// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/mounts_ride_others_tests.inc: other players see a player ride (Social, seeing other players: riders on their mounts)
//
//  Two or three players in one world, each with a session and a fake client (tests/mounts_ride_others_rig.h).  The rider summons a mount, gets on, rides, sprints, gets off and sends
//  the mount away; the watcher's client is read as it receives it: the mount (a simulated proxy that moves, with nothing of its owner's), the rider's pawn attached to it, and the order
//  of it all.  Also: a watcher who comes into range while the rider rides, a mount that is sent away, a relog, and two riders side by side.
// ============================================================================
#include "mounts_ride_others_rig.h"

/// A rider (cyb) and a watcher (ayla) in one world.  The watcher stands where the rider starts, or `watcher_dx` cm east of it.
struct Scene {
    Harness                  a;
    Players                  all;
    std::unique_ptr<Harness> b;
    explicit Scene(double watcher_dx = 0) : a(cyb_record()) {
        a.play();
        std::optional<std::pair<double, double>> at;
        if (watcher_dx != 0) {
            const GoodPosition g = a.s->movement().good();
            at = std::make_pair(g.x + watcher_dx, g.y);
        }
        b = join_world(a, all, other_record(other_id(2), "ayla", 4, at));
    }
    uint64_t pawn_id() const {
        const world::WorldActor* p = shown_pawn(a);
        return p ? p->id : 0;
    }
    /// The channel the watcher holds an actor on (0: none).
    uint32_t channel(uint64_t actor) const { return b->s->actor_view() ? b->s->actor_view()->channel_of(actor) : 0; }
};

/// Whether an object reference names something.
static bool names(const std::optional<emit::FIntrepidNetworkGUID>& g) { return g && g->is_valid(); }

static bool same_object(const std::optional<emit::FIntrepidNetworkGUID>& got, const emit::FIntrepidNetworkGUID& want) { return got && got->ObjectId == want.ObjectId; }

/// Whether the items of the fast array `field` of the object `object` in `s` name `guid` in their maps (InteractVisibleStateMap and InteractGameplayStateMap).
static bool map_names(const States& s, uint64_t object, const char* field, const std::array<uint32_t, 4>& guid) {
    const auto it = s.find(object);
    if (it == s.end() || !it->second.schema) return false;
    const schema::NetField* f = it->second.schema->field(field);
    const repl::FastArrayState* arr = f ? it->second.arrays.find(f->index) : nullptr;
    if (!arr) return false;
    for (const repl::FastArrayItem& item : arr->items()) {
        for (const auto& [index, value] : item.values.values) {
            const auto* w = value.get_if<repl::GuidToBoolWrapper>();
            if (!w) continue;
            for (const auto& [g, flag] : w->pairs) {
                if (g.a == guid[0] && g.b == guid[1] && g.c == guid[2] && g.d == guid[3]) return true;
            }
        }
    }
    return false;
}

/// How many items the fast array `field` of the objects of class `cls` holds, the most any one chain the watcher's client got on `channel` from `from` on carried.
static size_t array_items_sent(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from, const char* cls, const char* field) {
    size_t most = 0;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        for (const auto& [id, o] : chain_states(d, actor)) {
            if (!o.schema || std::string(o.schema->name) != cls) continue;
            const schema::NetField* f = o.schema->field(field);
            const repl::FastArrayState* arr = f ? o.arrays.find(f->index) : nullptr;
            if (arr) most = std::max(most, arr->items().size());
        }
    }
    return most;
}

static uint64_t id_of_component(const world::WorldActor& m, const char* cls) {
    const repl::Subobject* s = spawns::sub_of(m, cls);
    return s ? s->guid.ObjectId : 0;
}

/// The latest ReplicatedMovement the watcher's client was sent for the actor on `channel` since `from`, and the fastest speed any chain since then said.
struct MotionSeen {
    std::optional<SentMotion> last;
    double                    peak_speed = 0;
    size_t                    updates = 0;       // the chains that carried a ReplicatedMovement
    bool                      forward = false;   // some chain said the move input is held
};
static MotionSeen motion_seen(const Harness& watcher, uint32_t channel, const repl::Actor& actor, size_t from) {
    MotionSeen out;
    for (size_t i = from; i < watcher.got.size(); ++i) {
        const fake::Delivered& d = watcher.got[i];
        if (d.channel != channel || d.close) continue;
        const States st = chain_states(d, actor);
        if (const auto m = motion_in(st)) {
            out.last = m;
            out.peak_speed = std::max(out.peak_speed, std::hypot(m->vx, m->vy, m->vz));
            ++out.updates;
        }
        if (const repl::Value* v = value_in(st, 0, "CurrentMovementInput")) {
            if (const auto* input = v->get_if<repl::PlayerMovementInput>()) out.forward = out.forward || input->bits != 0;
        }
    }
    return out;
}

// ---- the mount, from the summon on ----------------------------------------------------------------------------------------------------

static void test_others_see_a_summoned_mount_and_its_owners_pawn_names_it() {
    std::printf("other players see a summoned mount: a simulated proxy of nobody's, and the pawn of its owner names it\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    summon_with(a, kMountSlot);
    a.run(milliseconds(1900));
    const world::WorldActor* m = a.mount();
    CHECK(m && a.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    if (!m) return;
    const uint32_t ch = sc.channel(m->id);
    CHECK(ch != 0 && b.s->actor_view()->state_of(m->id) == world::ClientState::Awake, "the watcher's view holds the rider's mount awake, on a channel of its own");
    const fake::Delivered* open = open_on(b, ch);
    CHECK(open != nullptr, "...and its client was sent the mount's open");
    if (!open) return;
    const States st = chain_states(*open, m->actor);
    const emit::FIntrepidNetworkGUID rider_pawn = a.s->bootstrap().setup().guids.pawn;
    CHECK(number_in(st, 0, "RemoteRole") == 1, "the open says RemoteRole 1: a simulated proxy (the owner's own copy is the autonomous 2 once it rides)");
    CHECK(!names(object_in(st, 0, "Owner")) && !names(object_in(st, 0, "Controller")) && !names(object_in(st, 0, "PlayerState")), "...with no Owner, Controller or PlayerState: they are objects of its owner's connection");
    CHECK(same_object(object_in(st, 0, "Instigator"), rider_pawn), "its Instigator is its owner's pawn, the one the watcher holds");
    CHECK(!names(object_in(st, id_of_component(*m, "UNPCSecondaryInfo"), "RiderPtr")), "nobody rides it yet: RiderPtr names none");
    const std::array<uint32_t, 4> ayla = b.s->bootstrap().setup().character_guid;
    CHECK(!map_names(st, id_of_component(*m, "UInteractableComponent"), "InteractVisibleStateMap", ayla) && !map_names(st, id_of_component(*m, "UInteractableComponent"), "InteractGameplayStateMap", ayla),
          "its interaction maps do not name the watcher: only its owner is offered to get on");
    // the summoning flag: on at the open, off 0.47 s later
    const auto flags = channel_state(b, ch, m->actor);
    CHECK(number_in(st, id_of_component(*m, "UMountComponent"), "bIsSummoning") == 1 && number_in(flags.merged, id_of_component(*m, "UMountComponent"), "bIsSummoning") == 0,
          "bIsSummoning is 1 in the open and 0 in the update that follows: the watcher sees the mount appear, as its owner does");
    // the owner's pawn, as the watcher holds it, names the mount
    const uint32_t pch = sc.channel(sc.pawn_id());
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(pch != 0 && pawn, "(the watcher holds the owner's pawn)");
    if (!pch || !pawn) return;
    const auto ps = channel_state(b, pch, pawn->actor);
    const uint64_t info_id = id_of_component(*pawn, "UNPCSecondaryInfo");
    CHECK(same_object(object_in(ps.merged, info_id, "MountablePet"), m->actor.guid), "the owner's pawn names the mount in its InteractInfo (MountablePet)");
    CHECK(!names(object_in(ps.merged, info_id, "MountPtr")), "...and rides nothing (MountPtr names none)");
    // the watcher is not offered the mount: it is its owner's alone
    const size_t refused = b.s->mount_interactions_refused();
    b.client_sends({b.client.actor(b.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
    b.run(milliseconds(100));
    CHECK(!b.s->mounted() && b.s->mount_interactions_refused() == refused + 1, "the watcher's own request to get on the mount is refused: it is not its mount");
}
AOC_REGISTER_TEST(test_others_see_a_summoned_mount_and_its_owners_pawn_names_it);

// ---- getting on, riding, the dash, getting off ---------------------------------------------------------------------------------------------

static void test_others_see_a_player_get_on_ride_sprint_and_get_off() {
    std::printf("other players see a player get on its mount, ride it, sprint and get off: the pawn attached to the mount, the mount moving, nothing of the owner's\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    summon_with(a, kMountSlot);
    a.run(milliseconds(1900));
    const world::WorldActor* m = a.mount();
    CHECK(m && a.s->mounts()->phase() == MountController::Phase::Out, "(the mount is out)");
    if (!m) return;
    walk_up_to_mount(a);
    const world::WorldActor* pawn = shown_pawn(a);
    const uint32_t pch = sc.channel(sc.pawn_id());
    const uint32_t mch = sc.channel(m->id);
    CHECK(pawn && pch != 0 && mch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!pawn || !pch || !mch) return;
    const emit::FIntrepidNetworkGUID mount_guid = m->actor.guid;
    const emit::FIntrepidNetworkGUID rider_guid = a.s->bootstrap().setup().guids.pawn;
    const uint64_t info_id = id_of_component(*pawn, "UNPCSecondaryInfo");
    const uint64_t rider_ptr_in = id_of_component(*m, "UNPCSecondaryInfo");
    const size_t before = b.got.size();
    get_on(a);
    a.run(milliseconds(300));
    CHECK(a.s->mounted(), "(the player is on the mount)");
    if (!a.s->mounted()) return;

    // getting on: ONE update of the rider's pawn attaches it, and it carries the export of the mesh it sits at
    const fake::Delivered* attach = nullptr;
    size_t attaching = 0;
    for (size_t i = before; i < b.got.size(); ++i) {
        const fake::Delivered& d = b.got[i];
        if (d.channel != pch || d.close) continue;
        if (names(object_in(chain_states(d, pawn->actor), 0, "AttachmentReplication.AttachParent"))) {
            attach = &d;
            ++attaching;
        }
    }
    CHECK(attach != nullptr && attaching == 1, "the watcher's client was sent exactly one update that attaches the rider's pawn");
    if (attach) {
        const States st = chain_states(*attach, pawn->actor);
        CHECK(same_object(object_in(st, 0, "AttachmentReplication.AttachParent"), mount_guid), "it is attached to the mount (AttachParent)");
        CHECK(names(object_in(st, 0, "AttachmentReplication.AttachComponent")) && exports_name(*attach, "CharacterMesh0"), "...at its CharacterMesh0, which the same update exports (the client has no other way to find it)");
        const repl::Value* socket = value_in(st, 0, "AttachmentReplication.AttachSocket");
        const auto* name = socket ? socket->get_if<repl::NameValue>() : nullptr;
        CHECK(name && name->text == "MountSocket", "...at the socket MountSocket");
        CHECK(number_in(st, 0, "AttachmentState.bAttachParentValid") == 1 && number_in(st, 0, "AttachmentState.InteractableZoneID") == 2 && number_in(st, 0, "AttachmentState.bClientLerpToAttachment") == 1,
              "the attachment state says the parent is valid, the zone is the mount's and the client lerps to it");
        CHECK(number_in(st, 0, "ReplicatedMovementMode") == 0, "the rider's movement mode is none: it follows its attachment");
        CHECK(same_object(object_in(st, info_id, "MountPtr"), mount_guid), "its InteractInfo says MountPtr is the mount");
    }
    // the mount, as the watcher holds it from the open on
    const auto mount_all = channel_state(b, mch, m->actor);
    CHECK(number_in(mount_all.merged, 0, "RemoteRole") == 1 && !names(object_in(mount_all.merged, 0, "Owner")) && !names(object_in(mount_all.merged, 0, "Controller")) && !names(object_in(mount_all.merged, 0, "PlayerState")),
          "the mount stays a simulated proxy with no Owner, Controller or PlayerState while it is ridden (the rider's own copy of it is the autonomous one)");
    CHECK(same_object(object_in(mount_all.merged, rider_ptr_in, "RiderPtr"), rider_guid), "its InteractInfo says RiderPtr is the rider's pawn: the pawn the watcher holds");
    const auto own = channel_state(a, a.mount_ch(), m->actor);
    CHECK(number_in(own.merged, 0, "RemoteRole") == 2 && names(object_in(own.merged, 0, "Owner")), "(the rider's own copy of the mount is the autonomous one, with its Owner: the two forms differ)");
    CHECK(array_items_sent(b, mch, m->actor, 0, "UAoCStatsComponent", "StatRepInt32Owner") == 0 && array_items_sent(b, mch, m->actor, 0, "UAoCStatsComponent", "StatRepInt32OwnerProxy") == 0,
          "the watcher is sent none of the rider's stats that the mount carries for its owner (StatRepInt32Owner, StatRepInt32OwnerProxy)");
    CHECK(array_items_sent(a, a.mount_ch(), m->actor, 0, "UAoCStatsComponent", "StatRepInt32Owner") > 0, "(the rider is: those arrays are its own)");
    const uint64_t ability_id = id_of_component(*m, "UAoCAbilityComponent");
    CHECK(value_in(mount_all.merged, ability_id, "KnownAbilities") == nullptr && value_in(own.merged, ability_id, "KnownAbilities") != nullptr, "the watcher is not sent the mount's dash (KnownAbilities) either; its rider is");

    // riding: the mount moves in the watcher's world, the rider's pawn does not move of itself
    Rider r = start_riding(a);
    const size_t ride_from = b.got.size();
    const double start_x = a.mount()->x;
    for (int i = 0; i < 20; ++i) ride_step(a, r, 99, 0);   // 19.8 m at 990 cm/s
    const MotionSeen riding = motion_seen(b, mch, m->actor, ride_from);
    const world::WorldActor* mm = a.mount();
    CHECK(mm && mm->x - start_x > 1500.0, "(the mount carried the rider more than 15 m)");
    CHECK(riding.updates >= 10, "the watcher's client was sent the mount's ReplicatedMovement in at least 10 updates while it rode");
    CHECK(riding.last && mm && std::fabs(riding.last->x - mm->x) < 150.0 && std::fabs(riding.last->y - mm->y) < 150.0, "...the last of them puts it where the mount is (within 1.5 m)");
    CHECK(riding.peak_speed > 900.0 && riding.peak_speed < 1100.0, "...with the speed the rider rode at: about 990 cm/s");
    CHECK(riding.forward, "...and the mount's move input held (CurrentMovementInput)");
    CHECK(chains_count(b, pch, ride_from) == 0, "the rider's own pawn was sent no update of its own while it rode (it is attached: the mount moves it)");
    a.run(milliseconds(400));
    const MotionSeen stopped = motion_seen(b, mch, m->actor, ride_from);
    CHECK(stopped.last && std::hypot(stopped.last->vx, stopped.last->vy) < 1.0, "the moves stopped: the mount is shown standing (its velocity 0) a moment later");

    // the dash: the effect and the speed are everyone's to see, the cooldown is its rider's
    const int64_t dash = a.fx.world->mounts()->dash_ability();
    const size_t dash_from = b.got.size();
    a.client_sends({a.client.actor(a.mount_ch(), trigger_on(a, ability_component_of(*a.mount()), dash, 21), true)});
    for (int i = 0; i < 20; ++i) ride_step(a, r, 133.65, 0);   // 1,336 cm/s: 990 x 1.35
    CHECK(a.s->dashes_started() == 1, "(the rider's dash started)");
    CHECK(array_items_sent(b, mch, m->actor, dash_from, "UAoCStatsComponent", "CurrentEffects") >= 1, "the watcher is sent the dash's effect on the mount (CurrentEffects)");
    CHECK(array_items_sent(a, a.mount_ch(), m->actor, dash_from, "UAoCAbilityComponent", "Cooldowns") >= 1 && array_items_sent(b, mch, m->actor, 0, "UAoCAbilityComponent", "Cooldowns") == 0,
          "...but not its cooldown, which is the rider's alone");
    const MotionSeen sprint = motion_seen(b, mch, m->actor, dash_from);
    CHECK(sprint.peak_speed > 1200.0 && sprint.peak_speed < 1450.0, "...and the mount's speed it was sent is the sprint's: about 1,336 cm/s");

    // getting off: one update takes the pawn off the mount, and the mount is left standing where it was
    a.run(milliseconds(300));
    const world::WorldActor* left = a.mount();
    const double left_x = left->x, left_y = left->y;
    const size_t off_from = b.got.size();
    get_off(a);
    a.run(milliseconds(100));
    CHECK(!a.s->mounted(), "(the rider is off)");
    const GoodPosition landed = a.s->movement().good();
    g_pawn_ts = 0.05f;
    move_pawn(a, move_at(g_pawn_ts, landed.x, landed.y, landed.z - 20, yaw16_of(landed.yaw_degrees), repl::kMoveFalling));
    a.run(milliseconds(400));
    const auto off = channel_state(b, pch, pawn->actor, off_from);
    CHECK(!names(object_in(off.merged, 0, "AttachmentReplication.AttachParent")) && !names(object_in(off.merged, 0, "AttachmentReplication.AttachComponent")), "the watcher's client was sent the pawn attached to nothing");
    CHECK(number_in(off.merged, 0, "AttachmentState.bAttachParentValid") == 0 && number_in(off.merged, 0, "AttachmentState.InteractableZoneID") == 0 && !names(object_in(off.merged, info_id, "MountPtr")),
          "...with the attachment state cleared and MountPtr none");
    CHECK(value_in(off.merged, 0, "RepDetachmentLocation") == nullptr, "...and not RepDetachmentLocation (the place the rider leaves the saddle is its own client's)");
    const auto place = motion_in(off.merged);
    CHECK(place && std::fabs(place->x - landed.x) < 150.0 && std::fabs(place->y - landed.y) < 150.0, "the pawn's ReplicatedMovement puts it where the rider landed, beside the mount");
    const MotionSeen after = motion_seen(b, mch, m->actor, dash_from);   // (the latest ReplicatedMovement the watcher was sent: the dismount adds none, the mount is where it stood)
    CHECK(!names(object_in(channel_state(b, mch, m->actor, off_from).merged, rider_ptr_in, "RiderPtr")), "the mount's RiderPtr is none again");
    CHECK(after.last && std::fabs(after.last->x - left_x) < 150.0 && std::fabs(after.last->y - left_y) < 150.0 && std::hypot(after.last->vx, after.last->vy) < 1.0, "the mount stays standing where its rider left it");
    CHECK(sc.channel(m->id) == mch && sc.channel(sc.pawn_id()) == pch, "(the watcher still holds both on the channels they were on)");
}
AOC_REGISTER_TEST(test_others_see_a_player_get_on_ride_sprint_and_get_off);

// ---- coming into range ---------------------------------------------------------------------------------------------------------------

static void test_a_watcher_who_comes_into_range_sees_the_rider_mounted() {
    std::printf("a player who comes into range while another rides sees it mounted, with the mount where it is now\n");
    Scene sc(30000);   // the watcher stands 300 m east of where the rider starts
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    CHECK(m && pawn && sc.channel(m->id) == 0 && sc.channel(pawn->id) == 0, "300 m away the watcher holds neither the rider's pawn nor its mount");
    if (!m || !pawn) return;
    const GoodPosition watcher_at = b.s->movement().good();
    Rider r = start_riding(a);
    int steps = 0;
    for (; steps < 400 && (sc.channel(m->id) == 0 || sc.channel(pawn->id) == 0); ++steps) ride_step(a, r, 99, 0);
    const uint32_t mch = sc.channel(m->id), pch = sc.channel(pawn->id);
    CHECK(mch != 0 && pch != 0, "riding toward the watcher, the rider and its mount come into its range");
    if (!mch || !pch) return;
    const double distance = std::hypot(a.mount()->x - watcher_at.x, a.mount()->y - watcher_at.y);
    CHECK(distance > 12000.0 && distance < 16000.0, "...at about 150 m, the range other players are shown in");
    a.run(milliseconds(100));
    // the open of the pawn says it is attached already, and the open of the mount says who rides it, and where it is
    const fake::Delivered* pawn_open = open_on(b, pch);
    const fake::Delivered* mount_open = open_on(b, mch);
    CHECK(pawn_open && mount_open, "(the watcher's client was sent an open of each)");
    if (!pawn_open || !mount_open) return;
    const States ps = chain_states(*pawn_open, pawn->actor);
    CHECK(same_object(object_in(ps, 0, "AttachmentReplication.AttachParent"), m->actor.guid) && same_object(object_in(ps, id_of_component(*pawn, "UNPCSecondaryInfo"), "MountPtr"), m->actor.guid),
          "the open of the rider's pawn attaches it to the mount, and its InteractInfo says MountPtr: it is seen mounted from the start");
    CHECK(exports_name(*pawn_open, "CharacterMesh0"), "...with the export of the mesh it sits at");
    const States ms = chain_states(*mount_open, m->actor);
    CHECK(number_in(ms, 0, "RemoteRole") == 1 && !names(object_in(ms, 0, "Owner")) && same_object(object_in(ms, id_of_component(*m, "UNPCSecondaryInfo"), "RiderPtr"), a.s->bootstrap().setup().guids.pawn),
          "the open of the mount is the simulated proxy's, with its rider named");
    const auto motion = motion_in(ms);
    const NewActorAt at = new_actor_at(mount_open->content);
    CHECK(motion && std::fabs(motion->x - a.mount()->x) < 3500.0 && std::fabs(motion->y - a.mount()->y) < 3500.0, "its ReplicatedMovement puts it where the mount is now, not where it was summoned");
    CHECK(at.x && at.y && std::fabs(*at.x - a.mount()->x) < 3500.0 && std::fabs(*at.y - a.mount()->y) < 3500.0, "...and so does the place it opens at");
    CHECK(array_items_sent(b, mch, m->actor, 0, "UAoCStatsComponent", "StatRepInt32Owner") == 0 && array_items_sent(b, mch, m->actor, 0, "UAoCStatsComponent", "StatRepInt32OwnerProxy") == 0 &&
              value_in(ms, id_of_component(*m, "UAoCAbilityComponent"), "KnownAbilities") == nullptr,
          "...without what the mount carries for its owner");
    // and it follows the rider from there
    const size_t from = b.got.size();
    for (int i = 0; i < 10; ++i) ride_step(a, r, 99, 0);
    const MotionSeen after = motion_seen(b, mch, m->actor, from);
    CHECK(after.updates >= 5 && after.last && std::fabs(after.last->x - a.mount()->x) < 150.0, "the watcher goes on being sent the mount's motion");
}
AOC_REGISTER_TEST(test_a_watcher_who_comes_into_range_sees_the_rider_mounted);

// ---- the mount is sent away -----------------------------------------------------------------------------------------------------------------

static void test_others_see_the_mount_sent_away() {
    std::printf("other players see a mount sent away: it dissolves and leaves, and its owner's pawn names it no more\n");
    {
        Scene sc;
        Harness& a = sc.a;
        Harness& b = *sc.b;
        summon_with(a, kMountSlot);
        a.run(milliseconds(1900));
        const world::WorldActor* m = a.mount();
        CHECK(m != nullptr, "(the mount is out)");
        if (!m) return;
        const uint64_t id = m->id;
        const uint32_t mch = sc.channel(id);
        const uint32_t pch = sc.channel(sc.pawn_id());
        const world::WorldActor* pawn = shown_pawn(a);
        const repl::Actor mount_copy = m->actor;
        const uint64_t flags_id = id_of_component(*m, "UMountComponent");
        const uint64_t info_id = pawn ? id_of_component(*pawn, "UNPCSecondaryInfo") : 0;
        const size_t from = b.got.size();
        desummon_with(a, kMountSlot);
        a.run(milliseconds(500));
        const auto dissolving = channel_state(b, mch, mount_copy, from);
        CHECK(number_in(dissolving.merged, flags_id, "bIsDesummoning") == 1, "the watcher's client is sent the dissolve (bIsDesummoning) at once");
        const MotionSeen run = motion_seen(b, mch, mount_copy, from);
        CHECK(run.updates >= 3 && run.peak_speed > 500.0, "...and the mount running off, as its owner sees it");
        a.run(milliseconds(3000));
        const auto closes = closes_on(b, mch, from);
        CHECK(mount_count(a) == 0 && closes.size() == 1 && closes[0] == 0 && sc.channel(id) == 0, "3.2 s after the request the mount leaves the world and the watcher's channel is closed with Destroyed");
        CHECK(pawn != nullptr, "(the rider's pawn is still in the world)");
        if (!pawn) return;
        const auto pawn_state = channel_state(b, pch, pawn->actor, from);
        CHECK(value_in(pawn_state.merged, info_id, "MountablePet") != nullptr && !names(object_in(pawn_state.merged, info_id, "MountablePet")), "the owner's pawn was sent MountablePet none: it names no mount any more");
    }
    {
        // sent away while ridden: the rider comes off and the mount dissolves in front of the watcher
        Scene sc;
        Harness& a = sc.a;
        Harness& b = *sc.b;
        CHECK(ride_on(a), "(the rider rides)");
        if (!a.s->mounted()) return;
        const world::WorldActor* m = a.mount();
        const world::WorldActor* pawn = shown_pawn(a);
        const uint32_t mch = sc.channel(m->id);
        const uint32_t pch = sc.channel(pawn->id);
        const repl::Actor pawn_copy = pawn->actor;
        const size_t from = b.got.size();
        desummon_with(a, kMountSlot);
        a.run(milliseconds(500));
        CHECK(!a.s->mounted(), "(the rider is off)");
        const auto off = channel_state(b, pch, pawn_copy, from);
        CHECK(!names(object_in(off.merged, 0, "AttachmentReplication.AttachParent")) && number_in(off.merged, 0, "AttachmentState.bAttachParentValid") == 0,
              "the watcher's client is sent the pawn taken off the mount");
        a.run(milliseconds(3000));
        CHECK(mount_count(a) == 0 && closes_on(b, mch, from).size() == 1, "...and the mount leaves the watcher's world");
    }
}
AOC_REGISTER_TEST(test_others_see_the_mount_sent_away);

// ---- a relog -----------------------------------------------------------------------------------------------------------------------------

static void test_others_see_a_rider_leave_and_come_back() {
    std::printf("other players see a rider log out (the pawn and the mount leave) and log in again (standing, no mount)\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a), "(the rider rides)");
    if (!a.s->mounted()) return;
    Rider r = start_riding(a);
    for (int i = 0; i < 20; ++i) ride_step(a, r, 99, 0);
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const uint64_t pawn_id = pawn->id;
    const uint32_t mch = sc.channel(m->id), pch = sc.channel(pawn_id);
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its mount)");
    if (!mch || !pch) return;
    const GoodPosition saved = a.s->last_position();
    const size_t from = b.got.size();
    a.s->finish("the test");
    a.run(milliseconds(200));
    CHECK(shown_pawn(a) == nullptr && mount_count(a) == 0, "(the pawn and the mount left the registry with the session)");
    const auto pawn_closes = closes_on(b, pch, from), mount_closes = closes_on(b, mch, from);
    CHECK(pawn_closes.size() == 1 && pawn_closes[0] == 0 && mount_closes.size() == 1 && mount_closes[0] == 0, "the watcher's channels of the rider's pawn and of its mount were closed with Destroyed");
    CHECK(sc.channel(pawn_id) == 0, "(the watcher holds neither)");
    // it logs in again, where it was saved: beside the mount, on its feet
    svc::Character again = cyb_record();
    again.has_position = true;
    again.position.x = saved.x;
    again.position.y = saved.y;
    again.position.z = saved.z;
    auto a2 = join_world(a, sc.all, std::move(again));
    a.run(milliseconds(400));
    const world::WorldActor* pawn2 = shown_pawn(*a2);
    CHECK(pawn2 != nullptr && !a2->s->mounted() && mount_count(a) == 0, "(the player is in the world again, on foot, with no mount)");
    if (!pawn2) return;
    const uint32_t pch2 = sc.channel(pawn2->id);
    CHECK(pch2 != 0, "the watcher is sent the returned player's pawn");
    const fake::Delivered* open = pch2 ? open_on(b, pch2) : nullptr;
    CHECK(open != nullptr, "...with an open");
    if (!open) return;
    const States st = chain_states(*open, pawn2->actor);
    CHECK(!names(object_in(st, 0, "AttachmentReplication.AttachParent")) && number_in(st, 0, "AttachmentState.bAttachParentValid") != 1, "...standing, attached to nothing");
}
AOC_REGISTER_TEST(test_others_see_a_rider_leave_and_come_back);

// ---- two riders side by side ---------------------------------------------------------------------------------------------------------------

/// Another rider's record: a character of its own whose items are its own too (the same kit, other instance guids).
static svc::Character rider_record(int n, const char* name) {
    svc::Character c = other_record(other_id(n), name, 10 + n);
    for (svc::InventoryItem& it : c.items) it.instance_guid[0] = static_cast<char>('b' + n);
    return c;
}

static void test_others_see_two_riders_side_by_side() {
    std::printf("other players see two riders side by side: each pawn on its own mount, each mount moving its own way\n");
    Harness a(cyb_record());
    a.play();
    Players all;
    auto c = join_world(a, all, rider_record(3, "kira"));
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4));
    CHECK(ride_on(a), "(the first rider rides)");
    CHECK(ride_on(*c), "(the second rider rides)");
    if (!a.s->mounted() || !c->s->mounted()) return;
    const world::WorldActor* ma = a.mount();
    const world::WorldActor* mc = c->mount();
    const world::WorldActor* pa = shown_pawn(a);
    const world::WorldActor* pc = shown_pawn(*c);
    CHECK(ma && mc && pa && pc && ma->id != mc->id && pa->id != pc->id, "(two mounts, two pawns)");
    if (!ma || !mc || !pa || !pc) return;
    const auto channel = [&](uint64_t id) { return b->s->actor_view()->channel_of(id); };
    const uint32_t ma_ch = channel(ma->id), mc_ch = channel(mc->id), pa_ch = channel(pa->id), pc_ch = channel(pc->id);
    CHECK(ma_ch && mc_ch && pa_ch && pc_ch && ma_ch != mc_ch && pa_ch != pc_ch, "the watcher holds the four actors, each on a channel of its own");
    if (!ma_ch || !mc_ch || !pa_ch || !pc_ch) return;
    const auto pa_state = channel_state(*b, pa_ch, pa->actor);
    const auto pc_state = channel_state(*b, pc_ch, pc->actor);
    const auto ma_state = channel_state(*b, ma_ch, ma->actor);
    const auto mc_state = channel_state(*b, mc_ch, mc->actor);
    CHECK(same_object(object_in(pa_state.merged, 0, "AttachmentReplication.AttachParent"), ma->actor.guid) && same_object(object_in(pc_state.merged, 0, "AttachmentReplication.AttachParent"), mc->actor.guid),
          "each rider's pawn is attached to its own mount");
    CHECK(same_object(object_in(ma_state.merged, id_of_component(*ma, "UNPCSecondaryInfo"), "RiderPtr"), a.s->bootstrap().setup().guids.pawn) &&
              same_object(object_in(mc_state.merged, id_of_component(*mc, "UNPCSecondaryInfo"), "RiderPtr"), c->s->bootstrap().setup().guids.pawn),
          "...and each mount names its own rider");
    // each rides its own way: the first east, the second north
    Rider ra = start_riding(a), rc = start_riding(*c);
    const size_t from = b->got.size();
    for (int i = 0; i < 20; ++i) {
        ra.ts += 0.1f;
        ra.x += 99;
        rc.ts += 0.1f;
        rc.y += 99;
        move_mount(a, move_at(ra.ts, ra.x, ra.y, ra.z, ra.yaw));
        move_mount(*c, move_at(rc.ts, rc.x, rc.y, rc.z, rc.yaw));
        a.step(milliseconds(100));
    }
    const MotionSeen east = motion_seen(*b, ma_ch, ma->actor, from);
    const MotionSeen north = motion_seen(*b, mc_ch, mc->actor, from);
    CHECK(east.last && std::fabs(east.last->vx) > 800.0 && std::fabs(east.last->vy) < 100.0, "the first mount's speed the watcher was sent is eastward");
    CHECK(north.last && std::fabs(north.last->vy) > 800.0 && std::fabs(north.last->vx) < 100.0, "the second mount's is northward");
    CHECK(east.last && north.last && std::fabs(east.last->x - a.mount()->x) < 150.0 && std::fabs(north.last->y - c->mount()->y) < 150.0, "each is where its mount is");
    // the riders see each other too: the other's mount in the form of a mount that is not its own
    const uint32_t seen_by_a = a.s->actor_view()->channel_of(mc->id);
    CHECK(seen_by_a != 0, "the first rider is sent the second's mount");
    if (seen_by_a) {
        const auto theirs = channel_state(a, seen_by_a, mc->actor);
        CHECK(number_in(theirs.merged, 0, "RemoteRole") == 1 && !names(object_in(theirs.merged, 0, "Owner")), "...as a simulated proxy with no Owner: its own mount's copy is the autonomous one");
        CHECK(a.mount_ch() != seen_by_a && number_in(channel_state(a, a.mount_ch(), ma->actor).merged, 0, "RemoteRole") == 2, "(and its own mount is still its own)");
    }
}
AOC_REGISTER_TEST(test_others_see_two_riders_side_by_side);

// ---- flying and swimming mounts --------------------------------------------------------------------------------------------------------------
//  What the other players are shown of a flying or a swimming mount is modelled: no recording has a third player in view of one.  The mount's movement mode (flying 5, swimming 4) and its
//  velocity, the vertical part included, are the ones its rider's moves say, as for a mount on the ground.

static void test_others_see_a_flying_mount() {
    std::printf("other players see a flying mount: the rider's pawn on it, the mount in flight (movement mode 5, its climb in the velocity), and left hovering where its rider gets off\n");
    Harness a(cyb_on_the_ground(), adopt_the_ground);
    a.play();
    Players all;
    auto b = join_world(a, all, other_record(other_id(2), "ayla", 4, std::make_pair(0.0, 0.0)));
    CHECK(ride_on(a, kFlyingSlot), "(the player rides a flyer)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const world::WorldActor* pawn = shown_pawn(a);
    const uint32_t mch = b->s->actor_view()->channel_of(m->id), pch = b->s->actor_view()->channel_of(pawn->id);
    CHECK(mch != 0 && pch != 0, "(the watcher holds the rider's pawn and its flying mount)");
    if (!mch || !pch) return;
    Rider r = start_riding(a);
    const size_t from = b->got.size();
    take_off(a, r, 4000.0);
    CHECK(a.s->moves_refused() == 0 && a.s->movement().good().movement_mode == repl::kMoveFlying, "(the rider took off by ordinary moves and flies)");
    const MotionSeen flight = motion_seen(*b, mch, m->actor, from);
    const auto state = channel_state(*b, mch, m->actor, from);
    CHECK(number_in(state.merged, 0, "ReplicatedMovementMode") == 5, "the watcher is sent the mount's movement mode: 5, flying");
    CHECK(flight.updates >= 10 && flight.last && std::fabs(flight.last->z - a.mount()->z) < 300.0 && flight.last->z > a.mount()->z - 4200.0, "...and its ReplicatedMovement puts it where the mount is, up in the air");
    double climb = 0;
    for (size_t i = from; i < b->got.size(); ++i) {
        if (b->got[i].channel != mch) continue;
        if (const auto mo = motion_in(chain_states(b->got[i], m->actor))) climb = std::max(climb, mo->vz);
    }
    CHECK(climb > 500.0, "...with its climb in the velocity (the vertical speed the rider's moves made)");
    const auto pawn_state = channel_state(*b, pch, pawn->actor);
    CHECK(same_object(object_in(pawn_state.merged, 0, "AttachmentReplication.AttachParent"), m->actor.guid), "the rider's pawn stays attached to the mount in the air");
    // getting off in the air: the mount hovers (its mode stays 5), only the rider falls
    const GoodPosition up = a.s->movement().good();
    const size_t off_from = b->got.size();
    get_off(a);
    a.run(milliseconds(400));
    CHECK(!a.s->mounted(), "(the rider is off in the air)");
    const auto off = channel_state(*b, pch, pawn->actor, off_from);
    CHECK(!names(object_in(off.merged, 0, "AttachmentReplication.AttachParent")) && number_in(off.merged, 0, "ReplicatedMovementMode") == 3, "the pawn is taken off the mount and falls (movement mode 3)");
    const world::WorldActor* hover = a.mount();
    const auto hover_state = channel_state(*b, mch, m->actor, off_from);
    const MotionSeen hovering = motion_seen(*b, mch, m->actor, from);
    CHECK(hover && std::fabs(hover->z - up.z) < 5.0 && hovering.last && std::hypot(hovering.last->vx, hovering.last->vy, hovering.last->vz) < 1.0 && std::fabs(hovering.last->z - up.z) < 300.0,
          "the mount hovers where its rider left it, standing in the watcher's world too");
    (void)hover_state;
}
AOC_REGISTER_TEST(test_others_see_a_flying_mount);

static void test_others_see_a_swimming_mount() {
    std::printf("other players see an aquatic mount in the water: the rider's pawn on it, the mount's movement mode 4 and its speed\n");
    Scene sc;
    Harness& a = sc.a;
    Harness& b = *sc.b;
    CHECK(ride_on(a, kAquaticSlot), "(the player rides an aquatic mount)");
    if (!a.s->mounted()) return;
    const world::WorldActor* m = a.mount();
    const uint32_t mch = sc.channel(m->id);
    CHECK(mch != 0, "(the watcher holds the aquatic mount)");
    if (!mch) return;
    Rider r = start_riding(a);
    const size_t from = b.got.size();
    for (int i = 0; i < 20; ++i) ride_step(a, r, 92, 0, i < 3 ? -30 : 0, 0.1, repl::kMoveSwimming);
    CHECK(a.s->mounted() && a.s->movement().good().movement_mode == repl::kMoveSwimming, "(the rider swims, and stays on)");
    const MotionSeen swim = motion_seen(b, mch, m->actor, from);
    const auto state = channel_state(b, mch, m->actor, from);
    CHECK(number_in(state.merged, 0, "ReplicatedMovementMode") == 4, "the watcher is sent the mount's movement mode: 4, swimming");
    CHECK(swim.updates >= 10 && swim.peak_speed > 800.0 && swim.peak_speed < 1100.0, "...and its speed in the water: about 920 cm/s");
    CHECK(swim.last && std::fabs(swim.last->x - a.mount()->x) < 150.0, "...and where it is");
}
AOC_REGISTER_TEST(test_others_see_a_swimming_mount);
