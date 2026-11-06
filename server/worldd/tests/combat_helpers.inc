// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_helpers.inc
//
//  the helpers of the combat tests that more than one test file uses: the wolf's archetype, the creature fights' rigs, the wire readers and the plans of the fights.  Included by test_worldd_combat.cpp inside its anonymous namespace, before combat_registered.inc.
// ============================================================================

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/combat_tests2.inc: a player's cast and hits on an NPC through the ability runtime, and an NPC's hit on the player
//  (included by test_worldd_combat.cpp inside its anonymous namespace, after combat_tests.inc)
// ============================================================================

std::string wolf_archetype(Fixture& f) {
    std::string wolf_path;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = path;
    }
    return wolf_path;
}

/// The Health the first hit of a level 10 Wolf takes from a player standing 6 m away, with the NPC damage multiplier at `mult` (NaN: the Wolf never hit).
static double wolf_first_hit_health(Fixture& f, double mult) {
    const double none = std::nan("");
    Rig rig(4, 20, {});
    if (!rig.ok()) return none;
    combat_rig::World w;
    if (!w.problem.empty()) return none;
    const std::string wolf_path = wolf_archetype(f);
    if (wolf_path.empty()) return none;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    cfg.npc_damage_mult = mult;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    if (w.spawn(wolf_path, 0, 0, 10, 900) == 0) return none;
    combat_rig::FakePlayer player(rig, 77, {600, 0, 1098});
    world.add_player(&player);
    double now = 0;
    while (now < 10.0 && player.hits.empty()) {
        now += 1.0 / 15.0;
        w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
        w.npcs->tick(now);
        world.tick(now);
    }
    if (player.hits.empty()) return none;
    for (const combat::ResolvedMod& m : player.hits.front().mods) {
        if (m.stat_guid == content::kGuidHealth) return m.value;
    }
    return none;
}

// ---- a Grem against a player who stands still: its own animation, its lunge, its bites and its way back ---------------------------------------------------------------------

/// The cast element of a creature's AbilityComponent as the world wrote it last: which element it is, its ability and its montage (0 once the montage is over).
struct GremCastView {
    int32_t  element = 0;
    int64_t  ability = 0;
    uint64_t montage = 0;
    aoc::protocol::emit::FIntrepidNetworkGUID target;       // the first of the element's Targets, in full
    aoc::protocol::emit::FIntrepidNetworkGUID instigator;   // ... and its Instigator
};

GremCastView grem_cast_view(world::ActorRegistry& registry, uint64_t actor) {
    GremCastView v;
    world::WorldActor* a = registry.find(actor);
    aoc::protocol::repl::FastArrayState* arr = a ? combat::npc_ability_detail::active_abilities(*a) : nullptr;
    if (!arr) return v;
    for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
        v.element = it.id;
        const auto g = it.values.values.find(1);
        if (g != it.values.values.end() && g->second.get_if<int64_t>()) v.ability = *g->second.get_if<int64_t>();
        const auto m = it.values.values.find(11);
        if (m != it.values.values.end() && m->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) v.montage = m->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()->ObjectId;
        const auto t = it.values.values.find(3);
        const aoc::protocol::repl::ArrayValue* targets = t != it.values.values.end() ? t->second.get_if<aoc::protocol::repl::ArrayValue>() : nullptr;
        v.target = {};
        if (targets && !targets->elements.empty()) {
            const auto first = targets->elements[0].values.find(1);
            if (first != targets->elements[0].values.end() && first->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) v.target = *first->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>();
        }
        const auto inst = it.values.values.find(17);
        v.instigator = inst != it.values.values.end() && inst->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() ? *inst->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : aoc::protocol::emit::FIntrepidNetworkGUID{};
    }
    return v;
}

/// One attack with its own animation, as the test saw it begin and end.
struct GremCast {
    double      start = 0, duration = 0;
    int64_t     ability = 0;
    uint64_t    montage_id = 0;
    std::string name;
    int         bites = 0;                              // the hit notifies of its montage that fire a hit of the ability
    double      surface_before = 0, angle_before = 0;   // the tree's two conditions as the world looked at them (the creature as it stood before it turned)
    double      facing_error = 0;                       // degrees between the facing the cast locked and the way to the player
    double      centre_start = 0, centre_end = 0;       // centre distance to the player when it began and when it was over
    int         delivered = 0;                          // hits of the ability that reached the player during it
    double      stepped_back = 0;                       // how far the Grem went back from the closest it came to the player during the cast
    double      closest = 0;                            // the closest it came to the player's centre during the cast
    bool        settled = true;                         // the player stood where it was for the whole cast
    int         mode_at_start = -1, mode_after = -1;    // the Grem's ReplicatedMovementMode when the cast began and shortly before it was over (the montage has let go by then)
    std::vector<double> notifies;                       // seconds after the start at which each bite is due
};

/// The creature tables the extractor's npcabilities step makes (tools/npcabilities). A tree that has none of the three files has not run that step: the test says so and is skipped. Tables that are
/// there and do not read are a failure of their own, not a skip: a check that quietly stops looking would pass on the very files it is for.
static bool load_creature_tables(combat::NpcAbilityData& tables) {
    std::string dir = NPC_MOTION_PATH;
    dir.erase(dir.find_last_of("/\\") + 1);
    const std::string files[3] = {dir + "npc_abilities.tsv", dir + "npc_montages.tsv", dir + "npc_targeting.tsv"};
    std::string err;
    if (tables.load(files[0], files[1], files[2], err)) return true;
    std::error_code ec;
    bool any_there = false;
    for (const std::string& file : files) any_there = any_there || std::filesystem::exists(file, ec);
    if (!any_there) {
        std::printf("  [skip] the creature tables (tools/npcabilities, the npcabilities extractor step): %s\n", err.c_str());
        return false;
    }
    CHECKF(false, "the creature tables are there but do not read: %s", err.c_str());
    return false;
}

static double wrap180(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}

// ---- a creature whose open has the client ignore its movement: every place, turn and montage the world writes reaches a client that applies it ---------------------------------

/// What one update of a creature's actor block says about its movement, as a client reads it.
struct MoveWire {
    double t = 0;
    int    rep_movement = -1;     // bReplicateMovement (-1: not in the block)
    bool   place = false;         // ReplicatedMovement (where it is, its facing)
    bool   lunge = false;         // RepRootMotion says a montage carries it
    double yaw = 0;               // the facing ReplicatedMovement says, in degrees
};

/// The blocks of the creature's own channel in the update the world has pending, read back as the client's connection would read them.
void read_move_wire(world::WorldActor& actor, aoc::protocol::repl::ActorShadow& shadow, double now, std::vector<MoveWire>& out) {
    std::vector<aoc::protocol::repl::ContentBlock> blocks;
    if (!aoc::protocol::repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    for (const aoc::protocol::repl::ContentBlock& b : blocks) {
        if (b.sub || !b.has_rep_layout) continue;
        const auto st = aoc::protocol::repl::load_object_state(*actor.actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        const aoc::protocol::schema::ClassSchema& sc = *actor.actor.obj.schema;
        MoveWire w;
        w.t = now;
        if (const auto* v = st->state.get(sc, "bReplicateMovement")) {
            if (v->get_if<bool>()) w.rep_movement = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
            if (v->get_if<bool>()) w.lunge = *v->get_if<bool>();
        }
        if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
            if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                w.place = true;
                w.yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
            }
        }
        out.push_back(w);
    }
}

// ---- fix batch 1: the reach of a bite whose record states none, the pause of a lunge, a lunge the AI's walk orders do not cut, a creature with nothing to play ---------------------------

/// What one update of a creature's actor block says about a lunge, as a client reads it.
struct LungeWire {
    double  t = 0;
    int     active = -1;       // RepRootMotion.bIsActive (-1: not in the block)
    int     obstructed = -1;   // bRootMotionObstructed
    double  position = -1;     // RepRootMotion.Position
    bool    has_mv_yaw = false, has_rrm_yaw = false;
    double  mv_yaw = 0, rrm_yaw = 0;   // the facing ReplicatedMovement (8 or 16 bits) and RepRootMotion.Rotation (16 bits) say, in degrees, as a client reads them
    bool    has_rrm_vz = false, has_rrm_z = false;
    double  rrm_vz = 0, rrm_z = 0;     // RepRootMotion.LinearVelocity's vertical part (cm/s) and RepRootMotion.Location's height (cm)
    bool    has_mv_speed = false;      // ReplicatedMovement names a velocity ...
    double  mv_speed = 0;              // ... and this is its length along the ground (cm/s)
    // the update of the creature's stats component (a block of its own): the crowd control state it says and the CurrentEffects items it adds or changes
    bool    stats_block = false;
    int     cc_flags = -1;             // CrowdControlState.StatusFlags (-1: the update does not say)
    bool    has_cc_snare = false;
    double  cc_snare = 0;              // ... and its SnareValue
    size_t  effect_items = 0;
    size_t  cc_fields = 0;             // how many fields of CrowdControlState the update carries (a client is told only what changed)
    // ... and the rest of the state a root, a freeze, a stun or a trip writes (-1 / false: the update does not say)
    int     cc_ocam = -1, cc_rooted_by_other = -1, cc_anim_request = -1;   // bOverrideClientAuthMovement, bRootedByOther, CurrentAnimationRequest (Stunned 0, Tripped 1, none 255)
    bool    has_cc_anim_record = false, has_cc_anim_start = false, has_cc_anim_duration = false, has_cc_anim_end = false;
    int64_t cc_anim_record = 0;        // CrowdControlRecordToAnimate
    double  cc_anim_start = 0, cc_anim_duration = 0, cc_anim_end = 0;   // SyncStartAnimTime, Duration, EndAnimTime
};

void read_lunge_wire(world::WorldActor& actor, aoc::protocol::repl::ActorShadow& shadow, double now, std::vector<LungeWire>& out) {
    std::vector<aoc::protocol::repl::ContentBlock> blocks;
    if (!aoc::protocol::repl::collect_update_blocks(actor.actor, shadow, blocks)) return;
    for (const aoc::protocol::repl::ContentBlock& b : blocks) {
        if (b.sub) {   // the stats component's own block: its crowd control state, and the effect items that come with it
            if (!b.has_rep_layout || !b.sub->obj.schema || std::string(b.sub->obj.schema->name) != "UAoCStatsComponent") continue;
            const aoc::protocol::schema::ClassSchema& ss = *b.sub->obj.schema;
            const auto sub_state = aoc::protocol::repl::load_object_state(ss, b.payload.data(), b.payload.bit_pos(), true);
            if (!sub_state) continue;
            LungeWire w;
            w.t = now;
            w.stats_block = true;
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.StatusFlags")) {
                if (v->get_if<uint64_t>()) w.cc_flags = static_cast<int>(*v->get_if<uint64_t>());
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.SnareValue")) {
                if (v->get_if<float>()) {
                    w.has_cc_snare = true;
                    w.cc_snare = *v->get_if<float>();
                }
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.bOverrideClientAuthMovement")) {
                if (v->get_if<bool>()) w.cc_ocam = *v->get_if<bool>() ? 1 : 0;
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.bRootedByOther")) {
                if (v->get_if<bool>()) w.cc_rooted_by_other = *v->get_if<bool>() ? 1 : 0;
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.CurrentAnimationRequest")) {
                if (v->get_if<uint64_t>()) w.cc_anim_request = static_cast<int>(*v->get_if<uint64_t>());
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.CrowdControlRecordToAnimate")) {
                if (v->get_if<int64_t>()) {
                    w.has_cc_anim_record = true;
                    w.cc_anim_record = *v->get_if<int64_t>();
                }
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.SyncStartAnimTime")) {
                if (v->get_if<double>()) {
                    w.has_cc_anim_start = true;
                    w.cc_anim_start = *v->get_if<double>();
                }
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.Duration")) {
                if (v->get_if<float>()) {
                    w.has_cc_anim_duration = true;
                    w.cc_anim_duration = *v->get_if<float>();
                }
            }
            if (const auto* v = sub_state->state.get(ss, "CrowdControlState.EndAnimTime")) {
                if (v->get_if<double>()) {
                    w.has_cc_anim_end = true;
                    w.cc_anim_end = *v->get_if<double>();
                }
            }
            if (const aoc::protocol::repl::FastArrayState* items = const_cast<aoc::protocol::repl::FastArrays&>(sub_state->arrays).get(ss, "CurrentEffects")) w.effect_items = items->items().size();
            for (const auto& [handle, value] : sub_state->state.values()) {
                const aoc::protocol::schema::RepCommand* cmd = ss.command(handle);
                if (cmd && std::string_view(cmd->path).rfind("CrowdControlState.", 0) == 0) ++w.cc_fields;
            }
            out.push_back(w);
            continue;
        }
        if (!b.has_rep_layout) continue;
        const auto st = aoc::protocol::repl::load_object_state(*actor.actor.obj.schema, b.payload.data(), b.payload.bit_pos(), true);
        if (!st) continue;
        const aoc::protocol::schema::ClassSchema& sc = *actor.actor.obj.schema;
        LungeWire w;
        w.t = now;
        if (const auto* v = st->state.get(sc, "RepRootMotion.bIsActive")) {
            if (v->get_if<bool>()) w.active = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "bRootMotionObstructed")) {
            if (v->get_if<bool>()) w.obstructed = *v->get_if<bool>() ? 1 : 0;
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.Position")) {
            if (v->get_if<float>()) w.position = *v->get_if<float>();
        }
        if (const auto* v = st->state.get(sc, "ReplicatedMovement")) {
            if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
                w.has_mv_yaw = true;
                w.mv_yaw = m->rotation.yaw * 360.0 / (st->opts.rep_movement_short_rotation ? 65536.0 : 256.0);
                const aoc::protocol::repl::Vec3d velocity = aoc::protocol::repl::dequantize_velocity(st->opts, m->linear_velocity);
                w.has_mv_speed = true;
                w.mv_speed = std::hypot(velocity.x, velocity.y);
            }
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.Rotation")) {
            if (const auto* r = v->get_if<aoc::protocol::repl::Rot>()) {
                w.has_rrm_yaw = true;
                w.rrm_yaw = r->yaw * 360.0 / 65536.0;
            }
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.LinearVelocity")) {
            if (const auto* q = v->get_if<aoc::protocol::repl::QuantVec>()) {
                w.has_rrm_vz = true;
                w.rrm_vz = aoc::protocol::repl::dequantize_vector(*q, 10).z;
            }
        }
        if (const auto* v = st->state.get(sc, "RepRootMotion.Location")) {
            if (const auto* q = v->get_if<aoc::protocol::repl::QuantVec>()) {
                w.has_rrm_z = true;
                w.rrm_z = aoc::protocol::repl::dequantize_vector(*q, 100).z;
            }
        }
        out.push_back(w);
    }
}

/// One tick of a fight between a creature and a player: where the creature was, where the player was, and what a client that has the creature open holds by then (whether a montage carries it,
/// whether it is held, how far the montage has played).
struct LungeSample {
    double   t = 0, wx = 0, wy = 0, px = 0, py = 0, position = -1;
    int      active = 0, obstructed = 0;
    int64_t  ability = 0;      // the ability of the cast element the creature's AbilityComponent holds
    uint64_t montage = 0;      // ... and its montage (0: none is playing)
    bool     cast_began = false;
    double   yaw = 0;          // the facing the server has
    double   mv_yaw = 0, rrm_yaw = 0;   // the facing a client holds by this tick: the last ReplicatedMovement and RepRootMotion.Rotation it was sent
    bool     has_mv_yaw = false, has_rrm_yaw = false;
    double   wz = 0;                    // the creature's height (its origin) on the server
    bool     has_rrm_vz = false;        // this tick's update names RepRootMotion.LinearVelocity ...
    double   rrm_vz = 0;                // ... and its vertical part (cm/s)
    // what the creature names by NetGUID at this tick, in full (ObjectId, ServerId, Randomizer): its cast element's first Target and Instigator, its CombatInfo's first ActiveTarget, and its own actor's NetGUID
    aoc::protocol::emit::FIntrepidNetworkGUID item_target, item_instigator, active_target, creature_guid;
    // what this tick's updates say of the creature's walk and of the crowd control on it (the stats component's own block): the speed a client is sent (-1: no velocity in this tick's updates), the state's
    // flags (-1: none said) and snare, the effect items that block adds or changes, and how many effects the world holds on the creature after the tick
    double   wire_speed = -1;
    int      cc_flags = -1;
    bool     has_cc_snare = false;
    double   cc_snare = 0;
    size_t   cc_items = 0, held_effects = 0, cc_fields = 0;
    // the rest of the crowd control state this tick's updates say (see LungeWire): the flags that tell the client its walk is overridden, the animation it plays and for how long
    int      cc_ocam = -1, cc_rooted_by_other = -1, cc_anim_request = -1;
    bool     has_cc_anim_record = false, has_cc_anim_start = false, has_cc_anim_duration = false, has_cc_anim_end = false;
    int64_t  cc_anim_record = 0;
    double   cc_anim_start = 0, cc_anim_duration = 0, cc_anim_end = 0;
};

/// What a test may do in a fight between two ticks (land an effect, send the creature home, take the player out of the world ...).  The step is called twice in every tick: first (`after` false, `sample`
/// null) once the player has been steered and before the worlds move, then (`after` true) once they have moved and the sample of the tick has been made.
struct LungeStepContext {
    double now = 0;
    bool   after = false;
    const LungeSample*        sample = nullptr;
    combat::CombatWorld&      world;
    combat_rig::World&        w;
    combat_rig::FakePlayer&   player;
    uint64_t                  creature = 0;
};
using LungeStep = std::function<void(LungeStepContext&)>;

static std::string creature_path(Fixture& f, const char* leaf) {
    std::string found;
    for (const auto& [path, row] : f.motion.rows()) {
        if (path.find(leaf) != std::string::npos) found = path;
    }
    return found;
}

/// A fight of one creature against one player, tick by tick, as the client that watches it would hold it.  `steer` is asked at every tick before the world moves (with the sample of the tick before)
/// and may put the player somewhere else; it returns true when it did.  `timid` makes the creature one that does not start a fight; `provoke_at` (if >= 0) is when the player starts hitting it;
/// `leash_cm` (if > 0) is how far from home the creature chases.
using LungeSteer = std::function<bool(double now, const LungeSample* prev, double& px, double& py, const world::WorldActor& creature)>;

static bool run_lunge_fight(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double seconds, double player_x, double player_y, const LungeSteer& steer,
                            std::vector<LungeSample>& out, std::vector<combat::IncomingHit>* hits = nullptr, bool timid = false, double provoke_at = -1, combat::CombatWorld::Stats* stats = nullptr,
                            double leash_cm = 0, const spawns::OpenTemplate* made = nullptr, double creature_x = 0, double creature_y = 0, const LungeStep& on_step = nullptr) {
    Rig rig(4, 20, {});
    if (!rig.ok()) return false;
    combat_rig::World w;
    if (!w.problem.empty()) return false;
    const auto bp = f.motion.find(path);
    if (!bp) return false;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    world.set_ability_data(&tables);
    const uint64_t creature = w.spawn(path, creature_x, creature_y, 7, 900, bp->half_height, made);
    if (creature == 0) return false;
    world::WorldActor* actor = w.registry.find(creature);
    if (timid) actor->npc.aggro_off = true;
    if (leash_cm > 0) actor->npc.leash_radius_cm = leash_cm;   // (a chase that goes far must not be given up)
    aoc::protocol::repl::ActorShadow shadow;
    {
        aoc::protocol::repl::ExportAcks acks;
        bool failed = false;
        const auto open = aoc::protocol::repl::open_actor(actor->actor, shadow, acks, &failed);
        if (!open.has_value() || failed) return false;
    }
    double px = player_x, py = player_y;
    combat_rig::FakePlayer player(rig, 77, {px, py, 1098});
    world.add_player(&player);
    const double dt = 1.0 / 15.0;
    GremCastView last;
    LungeSample prev;
    bool have_prev = false;
    int active = 0, obstructed = 0;
    double mv_yaw = 0, rrm_yaw = 0;
    bool has_mv_yaw = false, has_rrm_yaw = false;
    double now = 0, next_hit = provoke_at;
    while (now < seconds) {
        now += dt;
        if (steer && steer(now, have_prev ? &prev : nullptr, px, py, *w.registry.find(creature))) player.rig_fighter().move_to({px, py, 1098});
        if (next_hit >= 0 && now >= next_hit) {   // the player hits it, and goes on hitting it every second: the threat a hit gives
            next_hit = now + 1.0;
            world.add_threat(creature, 77, 100.0, now);
        }
        if (on_step) {
            LungeStepContext c{now, false, nullptr, world, w, player, creature};
            on_step(c);
        }
        if (const stats::StatValue* max_hp = rig.cs->find(content::kGuidMaxHealth)) rig.cs->set_current(content::kGuidHealth, max_hp->value);   // (the fight is about the creature, not the player's life; asked afresh: a hit rebuilds the stats)
        w.npcs->note_player(77, px, py, now);
        w.npcs->tick(now);
        world.tick(now);
        std::vector<LungeWire> wire;
        read_lunge_wire(*actor, shadow, now, wire);
        double position = -1, tick_vz = 0, tick_speed = -1, tick_snare = 0;
        bool tick_has_vz = false, tick_has_snare = false;
        int tick_flags = -1;
        size_t tick_items = 0, tick_fields = 0;
        LungeWire cc_said;   // what this tick's updates say of the rest of the crowd control state (the last update that names a field wins)
        for (const LungeWire& u : wire) {
            if (u.active >= 0) active = u.active;
            if (u.obstructed >= 0) obstructed = u.obstructed;
            if (u.position >= 0) position = u.position;
            if (u.has_mv_yaw) mv_yaw = u.mv_yaw, has_mv_yaw = true;
            if (u.has_rrm_yaw) rrm_yaw = u.rrm_yaw, has_rrm_yaw = true;
            if (u.has_rrm_vz) tick_vz = u.rrm_vz, tick_has_vz = true;
            if (u.has_mv_speed) tick_speed = u.mv_speed;
            if (u.stats_block) {
                if (u.cc_flags >= 0) tick_flags = u.cc_flags;
                if (u.has_cc_snare) tick_snare = u.cc_snare, tick_has_snare = true;
                tick_items += u.effect_items;
                tick_fields += u.cc_fields;
                if (u.cc_ocam >= 0) cc_said.cc_ocam = u.cc_ocam;
                if (u.cc_rooted_by_other >= 0) cc_said.cc_rooted_by_other = u.cc_rooted_by_other;
                if (u.cc_anim_request >= 0) cc_said.cc_anim_request = u.cc_anim_request;
                if (u.has_cc_anim_record) cc_said.has_cc_anim_record = true, cc_said.cc_anim_record = u.cc_anim_record;
                if (u.has_cc_anim_start) cc_said.has_cc_anim_start = true, cc_said.cc_anim_start = u.cc_anim_start;
                if (u.has_cc_anim_duration) cc_said.has_cc_anim_duration = true, cc_said.cc_anim_duration = u.cc_anim_duration;
                if (u.has_cc_anim_end) cc_said.has_cc_anim_end = true, cc_said.cc_anim_end = u.cc_anim_end;
            }
        }
        const GremCastView view = grem_cast_view(w.registry, creature);
        const world::WorldActor* g = w.registry.find(creature);
        LungeSample s;
        s.t = now;
        s.wx = g->x;
        s.wy = g->y;
        s.px = px;
        s.py = py;
        s.position = position;
        s.active = active;
        s.obstructed = obstructed;
        s.ability = view.ability;
        s.montage = view.montage;
        s.cast_began = view.montage != 0 && (view.element != last.element || last.montage == 0);
        s.yaw = g->yaw_degrees;
        s.mv_yaw = mv_yaw;
        s.rrm_yaw = rrm_yaw;
        s.has_mv_yaw = has_mv_yaw;
        s.has_rrm_yaw = has_rrm_yaw;
        s.wz = g->z;
        s.has_rrm_vz = tick_has_vz;
        s.rrm_vz = tick_vz;
        s.item_target = view.target;
        s.item_instigator = view.instigator;
        s.active_target = combat_rig::active_target_guid(*g);
        s.creature_guid = g->actor.guid;
        s.wire_speed = tick_speed;
        s.cc_flags = tick_flags;
        s.has_cc_snare = tick_has_snare;
        s.cc_snare = tick_snare;
        s.cc_items = tick_items;
        s.cc_fields = tick_fields;
        s.cc_ocam = cc_said.cc_ocam;
        s.cc_rooted_by_other = cc_said.cc_rooted_by_other;
        s.cc_anim_request = cc_said.cc_anim_request;
        s.has_cc_anim_record = cc_said.has_cc_anim_record;
        s.cc_anim_record = cc_said.cc_anim_record;
        s.has_cc_anim_start = cc_said.has_cc_anim_start;
        s.cc_anim_start = cc_said.cc_anim_start;
        s.has_cc_anim_duration = cc_said.has_cc_anim_duration;
        s.cc_anim_duration = cc_said.cc_anim_duration;
        s.has_cc_anim_end = cc_said.has_cc_anim_end;
        s.cc_anim_end = cc_said.cc_anim_end;
        s.held_effects = world.effect_count(creature);
        last = view;
        out.push_back(s);
        prev = s;
        have_prev = true;
        if (on_step) {
            LungeStepContext c{now, true, &out.back(), world, w, player, creature};
            on_step(c);
        }
    }
    if (hits) *hits = player.hits;
    if (stats) *stats = world.stats();
    return true;
}

/// The lunges in a fight's samples: runs of ticks a montage carries the creature in, `last` being the last of them (the tick after it is the one that wrote the end of the carry).
struct LungeRun {
    size_t first = 0, last = 0;
};

static std::vector<LungeRun> lunge_runs(const std::vector<LungeSample>& s, int64_t ability = 0) {
    std::vector<LungeRun> runs;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].active != 1 || (i > 0 && s[i - 1].active == 1)) continue;
        size_t j = i;
        while (j + 1 < s.size() && s[j + 1].active == 1) ++j;
        if ((ability == 0 || s[i].ability == ability) && j + 1 < s.size()) runs.push_back({i, j});   // (a lunge the fight ended in is left out)
    }
    return runs;
}

/// The montage data of the montage a cast element named, among the montages of the ability.
static const combat::MontageData* montage_of_cast(Fixture& f, const combat::NpcAbilityData& tables, int64_t ability, uint64_t montage) {
    const abilities::AbilityDef* def = f.catalog->ability(ability);
    if (!def) return nullptr;
    for (const content::MontageRow& m : def->montages) {
        const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (md && md->guid == montage) return md;
    }
    return nullptr;
}

// ---- fix batch 2: the chase of a player who walks away, turning at the creature's rate, and a swing that faces its target -----------------------------------------------------

/// What a fight between a creature and a player who runs away shows: whether the creature stops while it chases, when it is first within reach, what it attacks and what hits.
struct ChaseReport {
    size_t halts = 0, attacks = 0, hits = 0, ticks = 0;
    double first_attack_at = -1, closest_gap = 1e9, worst_turn = 0, run_speed = 0, turn_rate = 0;
    // the bites of the attacks that began (those whose notify fell due before the fight ended) and what became of them: by the kind of rule of their targeting record
    size_t any_due = 0, any_landed = 0, any_missed = 0, any_blocked = 0, any_crit = 0;   // records that measure nothing (the target wherever he stands)
    size_t cone_due = 0, cone_landed = 0;                                                  // records that measure (validity on)
};

/// The hit notifies of a cast that fire a hit which hurts an enemy: seconds after its start.
static std::vector<double> cast_bites(Fixture& f, const combat::NpcAbilityData& tables, int64_t ability, uint64_t montage) {
    std::vector<double> out;
    const abilities::AbilityDef* def = f.catalog->ability(ability);
    const combat::MontageData* md = montage_of_cast(f, tables, ability, montage);
    if (!def || !md) return out;
    for (const combat::MontageData::Hit& h : md->hits) {
        for (const content::AbilityHitRow& r : def->hits) {
            if (r.source != "hit" || r.key != h.key) continue;
            const abilities::HitDef* hit = f.catalog->hit(r.hit_guid);
            if (hit && (hit->row.applicable_targets & 2) && !hit->mods.empty()) out.push_back(std::max(0.1, h.time / md->rate_scale));
            break;
        }
    }
    return out;
}

/// The player runs along +y at `run_cm_s` (zig-zagging `zig_degrees` either side of it, the side changing every second, when that is not 0) from 5 m in front of the creature for `seconds`.
static bool run_chase(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, double run_cm_s, double zig_degrees, double seconds, ChaseReport& out) {
    constexpr double kPi = 3.14159265358979323846;
    const auto bp = f.motion.find(path);
    if (!bp) return false;
    const double dt = 1.0 / 15.0;
    const LungeSteer steer = [&](double now, const LungeSample*, double& x, double& y, const world::WorldActor&) {
        const double heading = 90.0 + (zig_degrees > 0 ? ((static_cast<int>(now) % 2) ? zig_degrees : -zig_degrees) : 0.0);
        x += run_cm_s * dt * std::cos(heading * kPi / 180.0);
        y += run_cm_s * dt * std::sin(heading * kPi / 180.0);
        return true;
    };
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    if (!run_lunge_fight(f, tables, path, seconds, 0, 500, steer, samples, &hits, false, -1, nullptr, 40000.0)) return false;
    out = ChaseReport{};
    out.run_speed = bp->max_run_speed;
    out.turn_rate = bp->rotation_rate_yaw;
    out.hits = hits.size();
    double last_cast_t = -10;
    for (size_t i = 1; i < samples.size(); ++i) {
        const LungeSample& s = samples[i];
        const double speed = std::hypot(s.wx - samples[i - 1].wx, s.wy - samples[i - 1].wy) / dt;
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - bp->radius - 34.0;
        out.closest_gap = std::min(out.closest_gap, gap);
        out.worst_turn = std::max(out.worst_turn, std::fabs(wrap180(s.yaw - samples[i - 1].yaw)));
        if (s.cast_began) {
            ++out.attacks;
            if (out.first_attack_at < 0) out.first_attack_at = s.t;
        }
        if (s.montage != 0) last_cast_t = s.t;
        // it is chasing: no montage plays (nor played in the last half second), the player is beyond any reach it has, and the fight has begun
        if (s.montage == 0 && s.t - last_cast_t > 0.5 && gap > 450.0 && s.t > 1.0) {
            ++out.ticks;
            if (speed < 0.05 * out.run_speed) ++out.halts;
        }
        // the bites of the attack that began now: each is due at its notify, and counts if that fell due before the fight ended
        if (s.cast_began) {
            const combat::BiteArea* area = tables.area_of(s.ability);
            const bool measures = area && area->gate() == combat::BiteArea::Gate::Validity;
            for (const double at : cast_bites(f, tables, s.ability, s.montage)) {
                if (s.t + at > seconds - 0.2) continue;
                size_t& due = measures ? out.cone_due : out.any_due;
                ++due;
            }
            const combat::MontageData* md = montage_of_cast(f, tables, s.ability, s.montage);
            const double span = md ? md->seconds(1.0) : 0.0;
            for (const combat::IncomingHit& h : hits) {
                if (h.source_guid != s.ability || h.at < s.t - 0.05 || h.at > s.t + span + 0.25) continue;
                if (measures) {
                    ++out.cone_landed;
                    continue;
                }
                ++out.any_landed;
                if (h.flags & stats::kHitMiss) ++out.any_missed;
                if (h.flags & stats::kHitBlock) ++out.any_blocked;
                if (h.flags & stats::kHitCrit) ++out.any_crit;
            }
        }
    }
    return true;
}

// ---- hostile humanoids: a bandit raider against a player who stands still ---------------------------------------------------------------------------------------------


/// The client finds a creature's target by NetGUID, in full: the Targets of the creature's cast element name the player's collision cylinder and its CombatInfo.ActiveTargets his pawn, each with the ServerId and
/// Randomizer of his session (a bare ObjectId names nothing the client knows, and its targeting then finds no one to fly a bolt at), and the element's Instigator is the creature's own NetGUID in full.
static void check_targets_named_in_full(const std::vector<LungeSample>& samples, const char* name) {
    constexpr uint64_t kPlayer = 77;   // (the rig's player)
    const aoc::protocol::emit::FIntrepidNetworkGUID pawn = combat_rig::rig_pawn_guid(kPlayer), cylinder = combat_rig::rig_cylinder_guid(kPlayer);
    size_t element_ticks = 0, wrong_target = 0, wrong_instigator = 0, combat_ticks = 0, wrong_active = 0;
    for (const LungeSample& s : samples) {
        if (s.ability != 0) {
            ++element_ticks;
            wrong_target += s.item_target == cylinder ? 0 : 1;
            wrong_instigator += s.item_instigator == s.creature_guid && s.creature_guid.ServerId != 0 && s.creature_guid.Randomizer != 0 ? 0 : 1;
        }
        if (s.active_target.ObjectId != 0) {
            ++combat_ticks;
            wrong_active += s.active_target == pawn ? 0 : 1;
        }
    }
    CHECKF(element_ticks > 0 && wrong_target == 0, "the Targets of the %s's cast name the player's collision cylinder in full (ServerId %u, Randomizer %#x): %zu of %zu ticks wrong", name, cylinder.ServerId, cylinder.Randomizer,
           wrong_target, element_ticks);
    CHECKF(wrong_instigator == 0, "the Instigator of the %s's cast is its own NetGUID in full (%zu of %zu ticks wrong)", name, wrong_instigator, element_ticks);
    CHECKF(combat_ticks > 0 && wrong_active == 0, "the %s's CombatInfo.ActiveTargets names the player's pawn in full (%zu of %zu ticks wrong)", name, wrong_active, combat_ticks);
}

/// The open the world makes for a bandit Blueprint (spawns::GenericNpcs, as worldd builds it): a recorded guard's open with the abilities of the Blueprint's tree in its list of records.
struct BanditOpen {
    std::optional<spawns::CapturedData> captured;
    std::optional<spawns::GenericNpcs>  generic;
    const spawns::OpenTemplate*         open = nullptr;
    spawns::Verdict                     why = spawns::Verdict::Generic;
};

static bool make_bandit_open(Fixture& f, const combat::NpcAbilityData& tables, const std::string& path, BanditOpen& out, bool with_tree_abilities = true) {
    std::string err;
    out.captured = spawns::CapturedData::load(err);
    if (!out.captured) return false;
    spawns::GenericConfig config;
    if (with_tree_abilities) config.abilities_of = [&tables](const std::string& archetype) {
        std::vector<int64_t> kit;
        if (const std::vector<combat::CreatureAbility>* tasks = tables.abilities_of(archetype)) {
            for (const combat::CreatureAbility& t : *tasks) {
                if (std::find(kit.begin(), kit.end(), t.ability) == kit.end()) kit.push_back(t.ability);
            }
        }
        return kit;
    };
    out.generic.emplace(*out.captured, &f.motion, config);
    content::NpcAssetRow asset;
    asset.archetype_path = path;
    out.open = out.generic->template_for(asset, &out.why);
    return out.open != nullptr;
}

// ---- creature projectiles: the bandit arsonists' fire bolts and the goblin shamans' bomb ----------------------------------------------------------------------------------------

/// The casts of a fight as the client sees them: the start of each montage element (when, its ability and its montage).
struct ShotCast {
    double      t;
    int64_t     ability;
    uint64_t    montage;
    std::string name;
};

static std::vector<ShotCast> shot_casts(Fixture& f, const std::vector<LungeSample>& samples) {
    std::vector<ShotCast> out;
    for (const LungeSample& s : samples) {
        if (!s.cast_began) continue;
        const abilities::AbilityDef* def = f.catalog->ability(s.ability);
        out.push_back({s.t, s.ability, s.montage, def ? def->row.name : std::string("?")});
    }
    return out;
}

/// What the hits of a fight say about its projectiles: each launch notify of each cast (when it leaves) is tied to the first projectile hit that reaches the player after it.
struct ShotReport {
    size_t              due = 0, landed = 0, stray = 0, not_projectile = 0;
    std::vector<double> after_cast;   // seconds from the start of the cast to the hit, for every launch that landed
    std::vector<double> flight;       // seconds from the launch notify to the hit
    double              earliest_after_launch = 1e9;
};

static ShotReport shot_report(Fixture& f, const combat::NpcAbilityData& tables, const std::vector<ShotCast>& casts, const std::vector<combat::IncomingHit>& hits, double seconds, double flight_limit) {
    ShotReport r;
    std::vector<bool> used(hits.size(), false);
    for (const ShotCast& c : casts) {
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (!md) continue;
        for (const combat::MontageData::Fire& fire : md->fires) {
            const bool counted = c.t + md->seconds(1.0) + flight_limit <= seconds - 0.3;   // (a launch the fight ended before it could land is not due)
            const double leaves = c.t + fire.time / md->rate_scale;
            if (counted) ++r.due;
            for (size_t i = 0; i < hits.size(); ++i) {
                if (used[i] || hits[i].source_type != content::kSourceTypeProjectile || hits[i].at < leaves - 1e-6 || hits[i].at > leaves + flight_limit) continue;
                used[i] = true;
                if (counted) ++r.landed;
                r.after_cast.push_back(hits[i].at - c.t);
                r.flight.push_back(hits[i].at - leaves);
                r.earliest_after_launch = std::min(r.earliest_after_launch, hits[i].at - leaves);
                break;
            }
        }
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        if (hits[i].source_type != content::kSourceTypeProjectile) ++r.not_projectile;
        else if (!used[i]) ++r.stray;
    }
    return r;
}

// ---- creatures made from a generic Blueprint: their own attacks play ---------------------------------------------------------------------------------------------------------------

/// What a fight of a creature made from a generic Blueprint has to show (check_generic_creature_animates).
struct GenericFightPlan {
    const char* leaf;         // the tail of the Blueprint's path (creature_path)
    const char* name;         // for the messages
    double      seconds;      // the length of the fight
    bool        timid;        // it notices nobody: the player hits it a second in to begin the fight
    size_t      montages;     // at least so many montages among the tasks the world can use, all of them readable
    double      travel;       // at least one of them carries the creature this far (cm)
    size_t      casts;        // at least so many casts in the fight
    size_t      due;          // at least so many hits due at montage notifies
};

/// A fight of 'plan.seconds' against a player who stands 8 m away, with the creature's open as the world makes it (its tree's abilities in the list of records) and then with the donor's own list:
/// every usable ability is in the ability data, its montages read, the creature plays them, each hit lands at its notify, the lunges stay inside what the montages carry and it does not drift.
static void check_generic_creature_animates(Fixture& f, const GenericFightPlan& plan, const std::function<void(const std::map<std::string, size_t>&)>& more = nullptr) {
    constexpr double kDt = 1.0 / 15.0;
    const double seconds = plan.seconds;
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    const std::string path = creature_path(f, plan.leaf);
    const auto bp = f.motion.find(path);
    CHECKF(!path.empty() && bp && tables.abilities_of(path) != nullptr, "the Blueprint table has the %s and the creature tables list its abilities", plan.name);
    if (path.empty() || !bp || !tables.abilities_of(path)) return;
    CHECKF(!spawns::fighting_humanoid(path), "the %s is a creature, not one of the humanoids whose fights are built by name", plan.name);
    // the client carries a creature by its montage's root translation times the scale of its mesh (USkeletalMeshComponent::ConvertLocalRootMotionToWorld), and the world does the same
    const double scale = bp->default_scale;

    // the abilities of the tasks the world can use are in the ability data, and their montages read (the ones that carry the creature too)
    double travel = 0;
    size_t montages = 0, unreadable = 0, absent = 0;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* def = f.catalog->ability(t.ability);
        if (!def) {
            ++absent;
            continue;
        }
        for (const content::MontageRow& m : def->montages) {
            const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
            if (!md) continue;
            ++montages;
            if (md->root_unreadable) ++unreadable;
            else if (!md->root_motion.empty()) travel = std::max(travel, std::hypot(md->root_motion.back().x, md->root_motion.back().y));
        }
    }
    std::printf("  %zu montages, %zu unreadable, %zu usable abilities missing from the ability data; the farthest montage carries it %.0f cm\n", montages, unreadable, absent, travel);
    CHECKF(absent == 0, "every ability of its usable tasks is in the ability data (%zu missing)", absent);
    CHECKF(montages >= plan.montages && unreadable == 0 && travel >= plan.travel, "its montages read and one carries the creature forward (%zu montages, %zu unreadable, %.0f cm)", montages, unreadable, travel);

    // the open the world makes: the tree's abilities are the records its ability component loads
    BanditOpen made;
    CHECKF(make_bandit_open(f, tables, path, made), "the world makes an open for the %s (verdict: %s)", plan.name, spawns::verdict_name(made.why));
    if (!made.open) return;
    {
        combat_rig::World probe;
        if (!probe.problem.empty()) return;
        const uint64_t id = probe.spawn(path, 0, 0, 7, 900, bp->half_height, made.open);
        const world::WorldActor* a = probe.registry.find(id);
        size_t listed = 0, wanted = 0;
        std::set<int64_t> seen;
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (!seen.insert(t.ability).second) continue;
            ++wanted;
            if (a && combat::npc_knows_ability(*a, t.ability)) ++listed;
        }
        CHECKF(a && wanted >= 1 && listed == wanted, "its open lists the %zu abilities of its tree (%zu listed)", wanted, listed);
    }

    // a fight against a player who stands 8 m away (a creature that notices nobody is hit a second in)
    std::vector<LungeSample> samples;
    std::vector<combat::IncomingHit> hits;
    combat::CombatWorld::Stats stats;
    const double provoke = plan.timid ? 1.0 : -1.0;
    CHECKF(run_lunge_fight(f, tables, path, seconds, 0, 800, nullptr, samples, &hits, plan.timid, provoke, &stats, 40000.0, made.open), "a fight of %.0f s against a player who stands 8 m away runs", seconds);
    if (samples.empty()) return;
    check_targets_named_in_full(samples, plan.name);
    struct Cast {
        double      t;
        int64_t     ability;
        uint64_t    montage;
        std::string label;
    };
    std::vector<Cast> casts;
    double chase_began = -1;
    for (const LungeSample& s : samples) {
        if (chase_began < 0 && std::hypot(s.wx, s.wy) > 1.0) chase_began = s.t;
        if (!s.cast_began) continue;
        const abilities::AbilityDef* def = f.catalog->ability(s.ability);
        casts.push_back({s.t, s.ability, s.montage, def ? def->row.name : std::string("?")});
    }
    std::map<std::string, size_t> by_name;
    for (const Cast& c : casts) ++by_name[c.label];
    std::string summary;
    for (const auto& [label, n] : by_name) summary += (summary.empty() ? "" : ", ") + label + " " + std::to_string(n);
    std::printf("  %zu casts in %.0f s (%s); %llu attacks; the chase began at %.2f s\n", casts.size(), seconds, summary.c_str(), static_cast<unsigned long long>(stats.npc_attacks), chase_began);
    CHECKF(casts.size() >= plan.casts, "it plays its own animations (%zu casts in %.0f s; with the single swing it would play none)", casts.size(), seconds);
    if (more) more(by_name);

    // every hit of a cast lands at its montage's notify, once, and no hit lands outside a cast
    size_t due = 0, landed = 0, stray = 0;
    double worst_delay = 0;
    std::vector<bool> used(hits.size(), false);
    for (const Cast& c : casts) {
        const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
        if (!md) continue;
        if (c.t + md->seconds(1.0) > seconds - 0.3) continue;   // (a cast the fight ended in)
        for (const double at : cast_bites(f, tables, c.ability, c.montage)) {
            ++due;
            for (size_t i = 0; i < hits.size(); ++i) {
                if (used[i] || hits[i].source_guid != c.ability) continue;
                const double delay = hits[i].at - (c.t + at);
                if (delay < -1e-6 || delay > 2 * kDt + 1e-6) continue;
                used[i] = true;
                ++landed;
                worst_delay = std::max(worst_delay, delay);
                break;
            }
        }
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        if (used[i]) continue;
        bool in_cast = false;
        for (const Cast& c : casts) {
            const combat::MontageData* md = montage_of_cast(f, tables, c.ability, c.montage);
            if (md && hits[i].source_guid == c.ability && hits[i].at >= c.t - 1e-6 && hits[i].at <= c.t + md->seconds(1.0) + 2 * kDt) in_cast = true;
        }
        if (!in_cast) ++stray;
    }
    std::printf("  %zu hits were due at their notifies, %zu landed (the latest %.3f s after its notify); %zu hits outside a cast\n", due, landed, worst_delay, stray);
    CHECKF(due >= plan.due && landed == due, "every hit of every cast lands at its montage's notify (%zu of %zu)", landed, due);
    CHECKF(stray == 0, "no hit lands outside a cast (%zu)", stray);

    // the lunge: each time a montage carries it, it goes no farther than the montage does, and some lunges carry it a metre or more
    size_t far = 0, over = 0;
    double most = 0;
    const std::vector<LungeRun> runs = lunge_runs(samples);
    for (const LungeRun& r : runs) {
        const LungeSample& from = samples[r.first > 0 ? r.first - 1 : r.first];
        const LungeSample& to = samples[r.last + 1];
        const double went = std::hypot(to.wx - from.wx, to.wy - from.wy);
        most = std::max(most, went);
        if (went >= 100.0) ++far;
        if (went > travel * scale + 30.0) ++over;   // (the tick before the carry may still be a step of its chase: 300 cm/s is 20 cm a tick)
    }
    std::printf("  %zu lunges, %zu of a metre or more, the longest %.0f cm\n", runs.size(), far, most);
    CHECKF(runs.size() >= 3 && far >= 1, "the montages carry it along their root motion (%zu lunges, %zu of a metre or more)", runs.size(), far);
    CHECKF(over == 0, "no lunge carries it farther than its montage does at its scale %.2f (%zu lunges over %.0f cm)", scale, over, travel * scale + 30.0);

    // in reach, and no drift once it is at him
    const double r_creature = bp->radius * scale, r_player = 34.0;
    double reach = 0;
    for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
        if (!t.unknown && !t.off && t.fight && t.min_distance <= 0 && t.max_distance > reach) reach = t.max_distance;
    }
    double worst_gap = 0;
    size_t drift_ticks = 0;
    for (const LungeSample& s : samples) {
        const double gap = std::hypot(s.wx - s.px, s.wy - s.py) - r_creature - r_player;
        if (chase_began >= 0 && s.t > chase_began + 12.0) {
            worst_gap = std::max(worst_gap, gap);
            if (gap > reach + travel * scale + 60.0) ++drift_ticks;
        }
    }
    std::printf("  reach %.0f cm; the surface gap was at most %.0f cm at any time twelve seconds after the chase began\n", reach, worst_gap);
    CHECKF(drift_ticks == 0, "once it is at him it never drifts away from a player who stands still (%zu ticks farther than reach + travel)", drift_ticks);

    // the lunge is smooth and stays on the ground: no step is longer than the fastest the montages carry it, the creature's height follows the ground (the plain here, the hill's flank below) and
    // never the montage, and every update of a lunge says its velocity has no vertical part (the retail server's is 0.0 in every recorded update of every lunge, on a slope too)
    {
        double v_max = 0;   // cm/s, at the creature's scale
        for (const combat::CreatureAbility& t : *tables.abilities_of(path)) {
            if (t.unknown || t.off || !t.fight) continue;
            const abilities::AbilityDef* def = f.catalog->ability(t.ability);
            if (!def) continue;
            for (const content::MontageRow& m : def->montages) {
                const combat::MontageData* md = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
                if (!md) continue;
                for (size_t i = 1; i < md->root_motion.size(); ++i) {
                    const double dt = md->root_motion[i].time - md->root_motion[i - 1].time;
                    if (dt >= 0.01) v_max = std::max(v_max, std::hypot(md->root_motion[i].x - md->root_motion[i - 1].x, md->root_motion[i].y - md->root_motion[i - 1].y) / dt * scale);
                }
            }
        }
        struct Smooth {
            size_t ticks = 0;
            double step = 0, dz = 0, vz = 0, zspan = 0;
            size_t vz_seen = 0;
        };
        const auto measure = [&](const std::vector<LungeSample>& s) {
            Smooth m;
            double zlo = 1e18, zhi = -1e18;
            for (size_t i = 1; i < s.size(); ++i) {
                if (s[i].active != 1 || s[i - 1].active != 1) continue;
                ++m.ticks;
                const double step = std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy);
                m.step = std::max(m.step, step);
                m.dz = std::max(m.dz, std::fabs(s[i].wz - s[i - 1].wz));
                zlo = std::min(zlo, s[i].wz);
                zhi = std::max(zhi, s[i].wz);
                if (s[i].has_rrm_vz) {
                    ++m.vz_seen;
                    m.vz = std::max(m.vz, std::fabs(s[i].rrm_vz));
                }
            }
            m.zspan = zhi >= zlo ? zhi - zlo : 0;
            return m;
        };
        const double step_limit = v_max * kDt * 1.05 + 1.0;
        const Smooth flat = measure(samples);
        std::printf("  on the plain: %zu ticks carried, the longest step %.1f cm (at most %.1f), the height moves at most %.2f cm a tick, the velocity's vertical part at most %.2f cm/s over %zu updates\n", flat.ticks, flat.step,
                    step_limit, flat.dz, flat.vz, flat.vz_seen);
        CHECKF(flat.ticks >= 8 && flat.step <= step_limit, "its steps along the montage stay smooth: none is longer than the fastest the montages carry it (%.1f of %.1f cm)", flat.step, step_limit);
        CHECKF(flat.dz < 0.5 && flat.vz < 0.5 && flat.vz_seen >= 4, "on the plain its height stays where the ground is, and the updates say the velocity has no vertical part (%.2f cm a tick, %.2f cm/s)", flat.dz, flat.vz);
        // the same fight on the hill's flank (a slope of 400 cm over 3,000, going up towards the player)
        std::vector<LungeSample> hill;
        if (run_lunge_fight(f, tables, path, seconds, 4300, 0, nullptr, hill, nullptr, plan.timid, provoke, nullptr, 40000.0, made.open, 3500, 0)) {
            const Smooth up = measure(hill);
            const double slope = 400.0 / 3000.0;
            std::printf("  on the hill: %zu ticks carried, the longest step %.1f cm, the height moves at most %.2f cm a tick (slope %.3f: at most %.2f), %.1f cm in all, the velocity's vertical part at most %.2f cm/s\n", up.ticks, up.step, up.dz,
                        slope, slope * up.step + 1.0, up.zspan, up.vz);
            CHECKF(up.ticks >= 8 && up.step <= step_limit, "on the hill its steps stay smooth too (%.1f of %.1f cm)", up.step, step_limit);
            CHECKF(up.dz <= slope * up.step + 1.0 && up.zspan > 5.0, "its height follows the ground's slope and nothing else (%.2f cm a tick at most for steps of %.1f cm, %.1f cm in all)", up.dz, up.step, up.zspan);
            CHECKF(up.vz < 0.5 && up.vz_seen >= 4, "and the updates of a lunge up the hill still say the velocity has no vertical part (%.2f cm/s over %zu updates)", up.vz, up.vz_seen);
        } else {
            CHECK(false, "the fight on the hill's flank runs");
        }
        // a lunge nobody stands in the way of: the player is put 7 m ahead of the creature for as long as a montage carries it, so that the freeze box stays empty, and the creature goes the
        // montage's whole travel at its scale (a goblin of scale 1.5 goes 1.5 times what its montage says, as the client's copy does)
        if (scale > 1.05) {
            constexpr double kPiFree = 3.14159265358979323846;
            const LungeSteer away = [&](double, const LungeSample* prev, double& px, double& py, const world::WorldActor& c) {
                if (!prev || prev->active != 1) return false;
                px = c.x + std::cos(c.yaw_degrees * kPiFree / 180.0) * 700.0;
                py = c.y + std::sin(c.yaw_degrees * kPiFree / 180.0) * 700.0;
                return true;
            };
            std::vector<LungeSample> free_run;
            if (run_lunge_fight(f, tables, path, seconds, 0, 800, away, free_run, nullptr, plan.timid, provoke, nullptr, 40000.0, made.open)) {
                double longest = 0;
                for (const LungeRun& r : lunge_runs(free_run)) {
                    const LungeSample& from = free_run[r.first > 0 ? r.first - 1 : r.first];
                    const LungeSample& to = free_run[r.last + 1];
                    longest = std::max(longest, std::hypot(to.wx - from.wx, to.wy - from.wy));
                }
                std::printf("  with nobody in the way the longest lunge goes %.0f cm: the farthest montage's %.0f cm at scale %.2f is %.0f\n", longest, travel, scale, travel * scale);
                CHECKF(longest >= 0.9 * travel * scale && longest <= travel * scale + 30.0, "a lunge nobody stands in the way of goes its montage's travel at the creature's scale (%.0f cm of %.0f)", longest, travel * scale);
            } else {
                CHECK(false, "the fight with nobody in the way runs");
            }
        }
    }

    // the donor's own list of records: the client would hold no record of the creature's abilities, so the world keeps the single swing
    BanditOpen donor;
    CHECKF(make_bandit_open(f, tables, path, donor, false), "the world makes an open for the %s without its tree's abilities (verdict: %s)", plan.name, spawns::verdict_name(donor.why));
    if (!donor.open) return;
    std::vector<LungeSample> plain;
    combat::CombatWorld::Stats plain_stats;
    CHECK(run_lunge_fight(f, tables, path, seconds, 0, 800, nullptr, plain, nullptr, plan.timid, provoke, &plain_stats, 40000.0, donor.open), "the same fight with the donor's own list of records runs");
    size_t plain_casts = 0;
    for (const LungeSample& s : plain) plain_casts += s.cast_began ? 1 : 0;
    std::printf("  with the donor's own records: %zu casts, %llu single swings\n", plain_casts, static_cast<unsigned long long>(plain_stats.npc_attacks));
    CHECKF(plain_casts == 0 && plain_stats.npc_attacks >= 3, "with the donor's own list it plays no montage and keeps the single swing (%zu casts, %llu swings)", plain_casts, static_cast<unsigned long long>(plain_stats.npc_attacks));
}

// ---- humanoid creatures walk up to a player who stands far away, at their speed, and do not stand and stare -----------------------------------------------------------------------------

/// A chase in numbers: when the creature first moved, when and from how far it first cast, how fast it went while the player was far and nothing was playing.
struct ChaseNumbers {
    double first_move = -1, first_cast = -1, gap_at_cast = -1, speed = 0, end_gap = -1;
    size_t far_ticks = 0;
};

static ChaseNumbers chase_numbers(const std::vector<LungeSample>& s, double dt) {
    ChaseNumbers n;
    std::vector<double> speeds;
    for (size_t i = 1; i < s.size(); ++i) {
        const double step = std::hypot(s[i].wx - s[i - 1].wx, s[i].wy - s[i - 1].wy) / dt;
        const double gap = std::hypot(s[i].wx - s[i].px, s[i].wy - s[i].py);
        if (n.first_move < 0 && step > 20.0) n.first_move = s[i].t;
        if (n.first_cast < 0 && s[i].cast_began) n.first_cast = s[i].t, n.gap_at_cast = gap;
        if (s[i].montage == 0 && gap > 700.0 && s[i].t > 1.0) speeds.push_back(step);
    }
    std::sort(speeds.begin(), speeds.end());
    n.far_ticks = speeds.size();
    n.speed = speeds.empty() ? 0.0 : speeds[speeds.size() / 2];
    if (!s.empty()) n.end_gap = std::hypot(s.back().wx - s.back().px, s.back().wy - s.back().py);
    return n;
}

/// What the world should make of one chase: the creature goes at `speed`, and from the moment it sets off it is at the player (or has cast) within the time the walk takes and two seconds.
static void check_chase(const char* name, const ChaseNumbers& n, double start_gap, double speed, bool stat_speed) {
    CHECKF(n.first_move >= 0 && n.far_ticks >= 10, "%s sets off after the player and walks (first move at %.2f s, %zu ticks far from him)", name, n.first_move, n.far_ticks);
    CHECKF(std::fabs(n.speed - speed) <= 0.02 * speed, "%s walks at %.0f cm/s: its Blueprint's run speed%s (%.0f)", name, speed, stat_speed ? " times its MoveSpeedMult" : "", n.speed);
    const double walk = n.first_cast >= 0 ? (start_gap - n.gap_at_cast) / speed : 1e9;
    CHECKF(n.first_cast >= 0 && n.first_cast - n.first_move <= walk + 2.0,
           "%s attacks within %.1f s of setting off: the walk to where it cast (%.0f cm away) takes %.1f s, it did not stop to wait (first cast %.2f s after it set off)", name, walk + 2.0, n.gap_at_cast, walk,
           n.first_cast - n.first_move);
}

/// A suffix for a folder of the temp directory that no other running copy of this program uses (two gates can run it at once): the clock and an address of this run.
std::string unique_temp_suffix() {
    int marker = 0;
    char text[64];
    std::snprintf(text, sizeof text, "%llx_%llx", static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&marker)));
    return text;
}
