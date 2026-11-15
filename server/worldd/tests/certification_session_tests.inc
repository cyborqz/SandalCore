// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/certification_session_tests.inc  (included by test_worldd_area_session.cpp through area_session_registered.inc)
//
//  The artisan certifier and the recipe items through real sessions on the fixture's content.db:
//
//    the promotion     a conversation with the fixture's certifier (a greeting and a menu whose lines carry the data's certification actions): Promote moves the
//                      profession one tier up when its level is the tier's cap, the XPTracker's LevelingCertificationsFastMap is sent with the new certification (read
//                      back from the client's copy), the conversation ends and the progress is saved
//    the refusals      a Promote below the cap, the MaximumAllowable of the tier reached, a Demote of a Novice: nothing changes, the window stays open, no XPTracker
//    the second choice the same Promote again after the first: refused as below the new tier's cap
//    two professions   two promotions one after the other, each its own profession
//    two players       two sessions at the same certifier: each moves only its own profession
//    the relog         a new session from the saved professions plays the new tier, and its join's XPTracker says so
//    the recipe item   a LearnRecipes item teaches its recipe, is consumed, is saved with the character and the client's OwnerInfo lists it; a second use is
//                      refused (the recipe is known) and the item stays; an item whose record names no recipe is refused and stays
// ============================================================================
#include "protocol/repl/fast_array.h"
#include "protocol/repl/leveling.h"

namespace cert {

inline constexpr int64_t kRoot = 6064634370000000001ll;       // the fixture's certifier conversation
inline constexpr int64_t kGreeting = 6064634370000000002ll;
inline constexpr int64_t kMenu = 6064634370000000003ll;
inline constexpr int64_t kOlderMenu = 6064634370000000004ll;   // the menu of a file an older contentc wrote: one bare SetCertificationLevel action

// the fixture's professions
inline constexpr int64_t kWeaponsmithing = 108752746862200ll;
inline constexpr int64_t kArmorsmithing = 108752746927739ll;
inline constexpr int64_t kAlchemy = 108752757020295ll;
inline constexpr int64_t kCooking = 108752757020296ll;
inline constexpr int64_t kMining = 108752769996425ll;
inline constexpr int64_t kLumberjacking = 108752769996426ll;
inline constexpr int64_t kHerbalism = 108752770061963ll;
inline constexpr int64_t kFishing = 108752770061964ll;
inline constexpr int64_t kHunting = 108752770061965ll;
inline constexpr int64_t kLtWeaponsmithing = 108651600696952ll;
inline constexpr int64_t kLtArmorsmithing = 109460729190016ll;

// the fixture's certification levels
inline constexpr int64_t kNovice = 108906339653240ll;
inline constexpr int64_t kApprentice = 108906340767353ll;

// the fixture's recipe items (the Tailoring Level 15 one, whose data names recipe 6064631200434094179, and a T3 one that names none)
inline constexpr int64_t kRecipeItem = 6064631200504938606ll;
inline constexpr int64_t kNoRecipeItem = 6064634597980045319ll;
inline constexpr int64_t kAnnalistChest = 6064631200434094179ll;

/// The tables the certification and the recipe items need, on the fixture's small content.db.
inline content_fixture::DbOptions options() {
    content_fixture::DbOptions o;
    o.profession_tables = true;
    o.dialogue_tables = true;
    o.recipe_tables = true;
    return o;
}

/// A profession row: `tier`, `level` and `xp` as the character service stores them.
static svc::ProfessionState row(int64_t profession, int tier, int level, int64_t xp) {
    svc::ProfessionState r;
    r.profession_guid = profession;
    r.certification_tier = tier;
    r.level = level;
    r.xp = xp;
    return r;
}

/// A Novice at the Novice cap (level 10, the XP of level 10's threshold) can be promoted.
static svc::ProfessionState at_the_novice_cap(int64_t profession) { return row(profession, 0, 10, 40000); }

/// The Bard's record with `rows` as its professions.
static svc::Character record_with(std::vector<svc::ProfessionState> rows) {
    svc::Character c = cyb_record();
    c.professions = std::move(rows);
    return c;
}

static repl::DesignDataId leveling_key(int64_t type) {
    repl::DesignDataId d;
    d.guid = type;
    d.type_id = content::kTypeLevelingTypeDef;
    return d;
}

/// The client's copy of the XPTracker, built from the blocks the server sends (as the items-session tests read it).
struct TrackerReader {
    repl::LevelingState state;
    size_t               absorbed = 0;   // the chains of `got` read so far
    size_t               updates = 0;    // the XPTracker blocks read
    void absorb(const fake::Delivered& d, const emit::FIntrepidNetworkGUID& tracker) {
        if (d.open || d.close || d.content.bit_pos() <= 8) return;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) return;
        for (const repl::BlockView& v : *views) {
            if (v.header.is_actor || v.header.object != tracker) continue;
            size_t p = v.payload_pos;
            const size_t end = v.payload_pos + v.payload_bits;
            const schema::ClassSchema& cls = state.schema();
            if (v.header.has_rep_layout && !repl::read_rep_block(cls, d.content.data(), end, p)) continue;
            if (p >= end) continue;
            const auto fields = repl::read_fast_array_fields(cls, d.content.data(), end, p);
            if (!fields) continue;
            for (const repl::FastArrayField& f : *fields) state.apply(f);
            ++updates;
        }
    }
    std::optional<int64_t> certification(int64_t type) const { return state.certification(leveling_key(type)); }
    std::optional<int64_t> experience(int64_t type) const { return state.experience(leveling_key(type)); }
};

/// The recipes the client's OwnerInfo holds (the list the station window filters by), as the server set it for the next OwnerInfo update.
static std::vector<int64_t> client_known_recipes(const Harness& h) {
    std::vector<int64_t> out;
    const repl::Subobject* owner = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.owner_info);
    if (!owner) return out;
    const repl::Value* v = owner->obj.state.get(*owner->obj.schema, "PlayerKnownRecipeIds");
    const repl::ArrayValue* a = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!a) return out;
    for (const repl::ElementState& e : a->elements) {
        const auto it = e.values.find(1);
        if (it != e.values.end() && it->second.get_if<int64_t>()) out.push_back(*it->second.get_if<int64_t>());
    }
    return out;
}

/// ServerItemUsed on a bag item (the item is in Backpack slot `slot` with the instance guid `guid_n(n)`).
static BunchWriter item_used(int slot, uint32_t n) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
    if (!rpc) return BunchWriter();
    const auto g = items::guid_of(guid_n(n));
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::RpcParams p;
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(3));   // EItemLocation::Backpack
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    return actor_rpc(pawn, "ServerItemUsed", p);
}

/// A session on the fixture's certifier world.
struct CertHarness : Harness {
    uint64_t      certifier = 0;
    TrackerReader tracker;
    explicit CertHarness(svc::Character r) : Harness(std::move(r), options()) {
        // the fixture's conversations: this world is not loaded through the area, so the book is handed over as the recorded dialogue tests do
        std::string why;
        const auto data = fx.content->dialogue_data(why);
        if (!data) {
            std::printf("  [FAIL] the fixture's conversations do not load: %s\n", why.c_str());
            std::exit(2);
        }
        fx.world->set_dialogue(dialogue::Book(std::move(*data)));
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) {
            std::printf("  [FAIL] no vendor template\n");
            std::exit(2);
        }
        certifier = fx.add(*vendor_t, kCx + 800, kCy + 300, false, 0, "Certifier", kRoot);   // 8.5 m
        start();
    }
    void play() {
        settle();
        request_phase_in();
        settle();
        run(milliseconds(1500));   // the view opens the people around the player (the click needs the NPC awake on the client)
        settle();
        absorb_tracker();
    }
    void absorb_tracker() {
        for (; tracker.absorbed < got.size(); ++tracker.absorbed) tracker.absorb(got[tracker.absorbed], s->bootstrap().setup().guids.xp_tracker);
    }
    /// The certifier is talked to and the menu line is shown.
    void open_menu() {
        const world::WorldActor* npc = fx.world->registry().find(certifier);
        client_request(interact(npc->npc.interact_component), pawn_ch);
        client_request(dialogue_choice(choice_of(kGreeting, 0)), pc_ch);
    }
    void choose(int32_t line) {
        client_request(dialogue_choice(choice_of(kMenu, line)), pc_ch);
        absorb_tracker();
    }
    /// The certifier is talked to and the greeting's second line (the older file's menu) is chosen.
    void open_older_menu() {
        const world::WorldActor* npc = fx.world->registry().find(certifier);
        client_request(interact(npc->npc.interact_component), pawn_ch);
        client_request(dialogue_choice(choice_of(kGreeting, 1)), pc_ch);
    }
    void choose_older(int32_t line) {
        client_request(dialogue_choice(choice_of(kOlderMenu, line)), pc_ch);
        absorb_tracker();
    }
};

}  // namespace cert

using namespace cert;

// ---- the promotion and its refusals -------------------------------------------------------------------------------------------------

static void test_certification_promotes_one_tier() {
    std::printf("the certifier: a Promote is one tier up, a Demote one down\n");
    CertHarness h(record_with({at_the_novice_cap(cert::kWeaponsmithing), at_the_novice_cap(cert::kArmorsmithing), at_the_novice_cap(cert::kAlchemy)}));
    h.play();
    CHECK(h.s->professions().valid() && h.s->professions().state(cert::kWeaponsmithing) == (professions::State{0, 10, 40000}), "the character is Novice level 10 in Weaponsmithing, at the cap");
    CHECK(h.tracker.certification(cert::kLtWeaponsmithing) == std::optional<int64_t>(cert::kNovice), "the client's XPTracker lists Weaponsmithing as Novice");

    h.open_menu();
    CHECK(h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu && h.s->talk().state().lines.size() == 10, "the greeting leads to the certifier's menu: ten lines");
    const size_t tracker_updates = h.tracker.updates;
    h.choose(0);   // Promote my Weaponsmithing certification please
    CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{1, 10, 40000}) && h.s->professions().can_use(cert::kWeaponsmithing, 1, 10),
          "Promote: Weaponsmithing is Apprentice, level 10, the XP kept at the cap");
    CHECK(!h.s->talk().open(), "the line ends the conversation");
    CHECK(h.tracker.certification(cert::kLtWeaponsmithing) == std::optional<int64_t>(cert::kApprentice) && h.tracker.updates > tracker_updates,
          "the client's XPTracker was sent the new certification (LevelingCertificationsFastMap)");
    CHECK(h.s->progress_changed() && !h.sink.progress.empty() && h.sink.progress.back().professions.size() == 22, "the progress is saved with the 22 profession rows");

    // the same choice again: an Apprentice at level 10 is below the Apprentice cap of 20
    h.open_menu();
    const size_t updates = h.tracker.updates;
    h.choose(0);
    CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{1, 10, 40000}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu,
          "the same Promote again: refused (level 10 is below the Apprentice cap), nothing changes and the window stays");
    CHECK(h.tracker.updates == updates, "a refused Promote sends no XPTracker");
    h.choose(1);   // Demote my Weaponsmithing certification please
    CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{0, 10, 40000}) && !h.s->talk().open(), "Demote: back to Novice level 10, the conversation ends");
    CHECK(h.tracker.certification(cert::kLtWeaponsmithing) == std::optional<int64_t>(cert::kNovice), "the client's XPTracker says Novice again");

    // two professions, one after the other
    h.open_menu();
    h.choose(2);   // Promote my Armorsmithing certification please
    CHECK(h.s->professions().state(cert::kArmorsmithing) == (professions::State{1, 10, 40000}), "Armorsmithing: Apprentice");
    h.open_menu();
    h.choose(4);   // Promote my Alchemy certification please
    CHECK(h.s->professions().state(cert::kAlchemy) == (professions::State{1, 10, 40000}), "Alchemy: Apprentice, in a second conversation");
    CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{0, 10, 40000}), "the professions not chosen are as they were");
}

static void test_certification_refusals() {
    std::printf("the certifier: a Promote below the cap, the tier's limit, a Demote of a Novice\n");
    {   // below the cap
        CertHarness h(record_with({row(cert::kWeaponsmithing, 0, 9, 32401)}));
        h.play();
        h.open_menu();
        h.choose(0);
        CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{0, 9, 32401}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu,
              "a Novice at level 9 promoted: refused, the window stays, the level is below the Novice cap of 10");
        h.choose(1);
        CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{0, 9, 32401}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu,
              "a Demote of a Novice: refused too, the window stays (Novice has no lower tier)");
    }
    {   // the MaximumAllowable: five Apprentices, then a sixth
        CertHarness h(record_with({row(cert::kWeaponsmithing, 1, 10, 40000), row(cert::kArmorsmithing, 1, 10, 40000), row(cert::kAlchemy, 1, 10, 40000),
                                   row(cert::kMining, 1, 10, 40000), row(cert::kFishing, 1, 10, 40000), at_the_novice_cap(cert::kCooking)}));
        h.play();
        h.open_menu();
        h.choose(6);   // Promote my Cooking certification please
        CHECK(h.s->professions().state(cert::kCooking) == (professions::State{0, 10, 40000}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu,
              "a sixth Apprentice: refused (the tier allows five), the window stays");
        h.choose(5);   // Demote my Alchemy certification please, one Apprentice down
        CHECK(h.s->professions().state(cert::kAlchemy) == (professions::State{0, 10, 40000}) && !h.s->talk().open(), "one Apprentice demoted: Alchemy is Novice again");
        h.open_menu();
        h.choose(6);
        CHECK(h.s->professions().state(cert::kCooking) == (professions::State{1, 10, 40000}), "with four Apprentices left, the sixth promotion is taken");
    }
}

/// The tier's limit counts the professions at the tier or above: a Journeyman was an Apprentice first.
static void test_certification_limit_counts_the_tier_and_above() {
    std::printf("the certifier: the tier's limit counts the professions at it or above\n");
    {   // four Apprentices and a Journeyman fill the five places of the Apprentice tier: a fifth Novice may not become an Apprentice (counting the Apprentices alone, 4, it would)
        CertHarness h(record_with({row(cert::kWeaponsmithing, 1, 10, 40000), row(cert::kAlchemy, 1, 10, 40000), row(cert::kMining, 1, 10, 40000), row(cert::kFishing, 1, 10, 40000),
                                   row(cert::kArmorsmithing, 2, 20, 160000), at_the_novice_cap(cert::kCooking)}));
        h.play();
        h.open_menu();
        h.choose(6);   // Promote my Cooking certification please
        CHECK(h.s->professions().state(cert::kCooking) == (professions::State{0, 10, 40000}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu,
              "four Apprentices and a Journeyman: a Novice may not become the sixth profession past Novice (the Apprentice tier counts the Journeyman)");
        h.choose(3);   // Demote my Armorsmithing certification please: the Journeyman goes down to Apprentice, which is still one of the five
        CHECK(h.s->professions().state(cert::kArmorsmithing).tier == 1 && !h.s->talk().open(), "the Journeyman is an Apprentice again");
        h.open_menu();
        h.choose(6);
        CHECK(h.s->professions().state(cert::kCooking).tier == 0 && h.s->talk().open(), "five Apprentices: still refused");
        h.choose(5);   // Demote my Alchemy certification please: an Apprentice goes down to Novice, which frees a place
        CHECK(h.s->professions().state(cert::kAlchemy).tier == 0 && !h.s->talk().open(), "an Apprentice is a Novice again");
        h.open_menu();
        h.choose(6);
        CHECK(h.s->professions().state(cert::kCooking) == (professions::State{1, 10, 40000}), "with four professions past Novice, the promotion is taken");
    }
    {   // three Journeymen and a Master are four at Journeyman or above, the Journeyman tier's limit: an Apprentice at the Apprentice cap may not go up
        CertHarness h(record_with({row(cert::kWeaponsmithing, 3, 30, 360000), row(cert::kArmorsmithing, 2, 20, 160000), row(cert::kAlchemy, 2, 20, 160000), row(cert::kCooking, 2, 20, 160000),
                                   row(cert::kMining, 1, 20, 160000)}));
        h.play();
        h.open_menu();
        h.choose(8);   // Promote my Mining certification please
        CHECK(h.s->professions().state(cert::kMining) == (professions::State{1, 20, 160000}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kMenu,
              "three Journeymen and a Master: a fifth profession may not become a Journeyman (the Journeyman tier counts the Master)");
        h.choose(1);   // Demote my Weaponsmithing certification please: the Master goes down to Journeyman, which is still one of the four
        CHECK(h.s->professions().state(cert::kWeaponsmithing).tier == 2 && !h.s->talk().open(), "the Master is a Journeyman again");
        h.open_menu();
        h.choose(8);
        CHECK(h.s->professions().state(cert::kMining).tier == 1 && h.s->talk().open(), "four Journeymen: still refused");
        h.choose(3);   // Demote my Armorsmithing certification please: a Journeyman goes down to Apprentice, which frees a place
        CHECK(h.s->professions().state(cert::kArmorsmithing).tier == 1 && !h.s->talk().open(), "a Journeyman is an Apprentice again");
        h.open_menu();
        h.choose(8);
        CHECK(h.s->professions().state(cert::kMining) == (professions::State{2, 20, 160000}), "with three Journeymen or better, the promotion is taken");
    }
}

/// The edges: no tier above GrandMaster, a Demote from the middle of a tier holds the level and the XP at the lower tier's cap (and the client's XPTracker says so), and a file an
/// older contentc wrote (a bare action that names no profession) leaves the line as it was before the certification.
static void test_certification_edges() {
    std::printf("the certifier: a GrandMaster, a Demote from the middle of a tier, the older file's bare action\n");
    {
        CertHarness h(record_with({row(cert::kWeaponsmithing, 4, 45, 810000)}));
        h.play();
        h.open_menu();
        const size_t updates = h.tracker.updates;
        h.choose(0);
        CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{4, 45, 810000}) && h.s->talk().open() && h.tracker.updates == updates,
              "a GrandMaster promoted: refused (there is no higher tier), the window stays, nothing is sent");
    }
    {
        CertHarness h(record_with({row(cert::kArmorsmithing, 1, 17, 115600)}));   // an Apprentice at level 17 (400 x 17 x 17 XP)
        h.play();
        h.open_menu();
        h.choose(3);   // Demote my Armorsmithing certification please
        CHECK(h.s->professions().state(cert::kArmorsmithing) == (professions::State{0, 10, 48399}) && !h.s->talk().open(),
              "an Apprentice at level 17 demoted: Novice, the level held at the Novice cap of 10 and the XP at the last point of level 10 (400 x 11 x 11 - 1)");
        CHECK(h.tracker.certification(cert::kLtArmorsmithing) == std::optional<int64_t>(cert::kNovice) && h.tracker.experience(cert::kLtArmorsmithing) == std::optional<int64_t>(48399),
              "the client's XPTracker says Novice with 48,399 XP");
    }
    {
        CertHarness h(record_with({at_the_novice_cap(cert::kWeaponsmithing)}));
        h.play();
        h.open_older_menu();
        CHECK(h.s->talk().open() && h.s->talk().state().shown_node == cert::kOlderMenu, "the greeting's second line leads to the older file's menu");
        const size_t updates = h.tracker.updates;
        h.choose_older(0);
        CHECK(h.s->professions().state(cert::kWeaponsmithing) == (professions::State{0, 10, 40000}) && h.s->talk().open() && h.s->talk().state().shown_node == cert::kOlderMenu &&
                  h.tracker.updates == updates,
              "a bare SetCertificationLevel (no profession): skipped as before version 44, the window stays, nothing changes");
    }
}

// ---- a second player at the same certifier -------------------------------------------------------------------------------------------

/// A second session on the same world: its own connection, its own client, the same certifier.
struct PeerSession {
    CertHarness&                       host;
    Connection                         conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client                       client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession>     s;
    std::vector<fake::Delivered>       got;
    uint32_t                           pawn_ch = 0, pc_ch = 0;
    std::string                        error;
    explicit PeerSession(CertHarness& h, svc::Character r) : host(h) {
        s = PlayerSession::join(conn, *host.fx.world, r, identity(), world_time_at(host.t), error);
        if (!s) {
            std::printf("  [FAIL] the second session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
    }
    void step() {
        host.t += milliseconds(20);   // (the host's clock; its own session is not ticked between the peer's steps)
        s->tick(conn, host.t);
        for (auto& pkt : conn.flush(host.t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
        s->take_acks(conn);
        client_sends({});
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        for (auto& pkt : client.packets(std::move(bunches))) {
            const auto in = conn.receive(pkt, host.t);
            s->take_acks(conn);
            for (const InBunch& b : in) s->on_bunch(conn, b, host.t);
            s->end_of_datagram(conn);
        }
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void play() {
        settle();
        client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
        settle();
        for (int i = 0; i < 75; ++i) step();   // the view opens the people around the player
        settle();
    }
    void request(const BunchWriter& rpc, uint32_t channel) {
        client_sends({client.actor(channel, rpc, true)});
        settle();
    }
};

static void test_certification_second_player() {
    std::printf("the certifier: a second player at the same NPC\n");
    CertHarness a(record_with({at_the_novice_cap(cert::kWeaponsmithing), at_the_novice_cap(cert::kArmorsmithing)}));
    a.play();
    svc::Character second = record_with({at_the_novice_cap(cert::kWeaponsmithing), at_the_novice_cap(cert::kArmorsmithing)});
    second.id = "0000000000000000000000000000c0b0";
    second.name = "bob";
    PeerSession b(a, second);
    b.play();

    CHECK(b.s->talk().open() == false, "the second player has joined and has no conversation open");
    const world::WorldActor* npc = a.fx.world->registry().find(a.certifier);
    a.open_menu();
    b.request(interact(npc->npc.interact_component), b.pawn_ch);
    b.request(dialogue_choice(choice_of(cert::kGreeting, 0)), b.pc_ch);
    CHECK(a.s->talk().open() && b.s->talk().open() && a.s->talk().state().shown_node == cert::kMenu && b.s->talk().state().shown_node == cert::kMenu,
          "both players have the certifier's menu open at the same time");

    a.choose(0);   // a promotes Weaponsmithing
    b.request(dialogue_choice(choice_of(cert::kMenu, 2)), b.pc_ch);   // b promotes Armorsmithing
    CHECK(a.s->professions().state(cert::kWeaponsmithing).tier == 1 && a.s->professions().state(cert::kArmorsmithing).tier == 0, "the first player's promotion moved only his Weaponsmithing");
    CHECK(b.s->professions().state(cert::kArmorsmithing).tier == 1 && b.s->professions().state(cert::kWeaponsmithing).tier == 0 && b.s->talk().open() == false,
          "the second player's promotion moved only his Armorsmithing");
}

// ---- the relog -----------------------------------------------------------------------------------------------------------------------

static void test_certification_survives_a_relog() {
    std::printf("the certifier: the new tier after a relog\n");
    CertHarness h(record_with({at_the_novice_cap(cert::kWeaponsmithing)}));
    h.play();
    h.open_menu();
    h.choose(0);
    CHECK(h.s->professions().state(cert::kWeaponsmithing).tier == 1 && !h.sink.progress.empty(), "the promotion is played and saved");
    // the character service's rows, as the save left them
    svc::Character relog = record_with({});
    if (!h.sink.progress.empty()) {
        for (const svc::ProfessionState& r : h.sink.progress.back().professions) {
            if (r.profession_guid == cert::kWeaponsmithing) relog.professions.push_back(r);
        }
    }
    CHECK(relog.professions.size() == 1 && relog.professions[0].certification_tier == 1, "the saved row is Apprentice");
    CertHarness again(relog);
    again.play();
    CHECK(again.s->professions().state(cert::kWeaponsmithing) == (professions::State{1, 10, 40000}), "a new session from the saved rows: Weaponsmithing Apprentice, level 10");
    CHECK(again.tracker.certification(cert::kLtWeaponsmithing) == std::optional<int64_t>(cert::kApprentice), "the join's XPTracker lists Apprentice");
}

// ---- the recipe items ----------------------------------------------------------------------------------------------------------------

/// The Bard's kit and three recipe items in bag slots 2, 3 and 4 (guids 302, 303 and 304): the Tailoring Level 15 recipe twice (to show an already-known recipe), and a
/// T3 item whose record names no recipe.
static svc::Character recipe_record(std::vector<svc::ProfessionState> rows) {
    std::vector<svc::InventoryItem> items = kit();
    items.push_back(make_item(Bag, 2, cert::kRecipeItem, 302));
    items.push_back(make_item(Bag, 3, cert::kRecipeItem, 303));
    items.push_back(make_item(Bag, 4, cert::kNoRecipeItem, 304));
    svc::Character c = cyb_record(std::move(items));
    c.professions = std::move(rows);
    return c;
}

static void test_recipe_item_teaches_its_recipe() {
    std::printf("the recipe item: learned, consumed, saved, and listed to the client\n");
    CertHarness h(recipe_record({row(108752746993277ll, 1, 10, 40000)}));   // Tailoring Apprentice, the item's own requirement
    h.play();
    CHECK(h.s->professions().state(108752746993277ll).tier == 1, "the character is Apprentice in Tailoring");
    CHECK(h.s->taught_recipes().empty() && client_known_recipes(h).empty(), "no recipe is known yet");
    const size_t applied = h.s->uses_applied();
    h.client_request(item_used(2, 302), h.pawn_ch);
    CHECK(h.s->uses_applied() == applied + 1, "the recipe item is used");
    CHECK(h.s->taught_recipes() == std::vector<int64_t>({cert::kAnnalistChest}), "it teaches its recipe (LearnRecipes)");
    CHECK(h.s->inventory().at(Bag, 2) == nullptr && h.s->inventory().at(Bag, 3) != nullptr, "the used stack is consumed as the data says (ConsumeOnUse); the second copy is kept");
    CHECK(client_known_recipes(h) == std::vector<int64_t>({cert::kAnnalistChest}), "the client's OwnerInfo lists the recipe: the station window shows it");
    CHECK(h.sink.known_recipes == std::vector<int64_t>({cert::kAnnalistChest}), "the learned recipe is saved with the character");

    // a relog carries the learned recipe (the character service's known rows)
    svc::Character relog = recipe_record({row(108752746993277ll, 1, 10, 40000)});
    relog.known_recipes = h.sink.known_recipes;
    CertHarness again(relog);
    again.play();
    CHECK(again.s->taught_recipes() == std::vector<int64_t>({cert::kAnnalistChest}), "a new session from the saved rows knows the recipe");
    CHECK(client_known_recipes(again) == std::vector<int64_t>({cert::kAnnalistChest}), "and the join's OwnerInfo lists it");
}

static void test_recipe_item_refusals() {
    std::printf("the recipe item: the requirement, an already-known recipe, an item with no recipe\n");
    {   // a Novice may not use the Apprentice Tailoring item
        CertHarness h(recipe_record({}));
        h.play();
        h.client_request(item_used(2, 302), h.pawn_ch);
        CHECK(h.s->taught_recipes().empty() && h.s->inventory().at(Bag, 2) != nullptr, "a Novice: the recipe needs Apprentice Tailoring, nothing is learned and the item stays");
    }
    {   // the second copy, after the first taught the recipe
        CertHarness h(recipe_record({row(108752746993277ll, 1, 10, 40000)}));
        h.play();
        h.client_request(item_used(2, 302), h.pawn_ch);
        CHECK(h.s->taught_recipes() == std::vector<int64_t>({cert::kAnnalistChest}), "the first item teaches the recipe");
        h.client_request(item_used(3, 303), h.pawn_ch);
        CHECK(h.s->taught_recipes() == std::vector<int64_t>({cert::kAnnalistChest}) && h.s->inventory().at(Bag, 3) != nullptr, "the same recipe again: refused, the second item stays");
        h.client_request(item_used(4, 304), h.pawn_ch);
        CHECK(h.s->taught_recipes() == std::vector<int64_t>({cert::kAnnalistChest}) && h.s->inventory().at(Bag, 4) != nullptr, "an item whose record names no recipe: refused, it stays");
    }
}

AOC_REGISTER_TEST(test_certification_promotes_one_tier);
AOC_REGISTER_TEST(test_certification_refusals);
AOC_REGISTER_TEST(test_certification_limit_counts_the_tier_and_above);
AOC_REGISTER_TEST(test_certification_edges);
AOC_REGISTER_TEST(test_certification_second_player);
AOC_REGISTER_TEST(test_certification_survives_a_relog);
AOC_REGISTER_TEST(test_recipe_item_teaches_its_recipe);
AOC_REGISTER_TEST(test_recipe_item_refusals);
