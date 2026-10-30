// ============================================================================
//  quest/session_objects.inc
//
//  Quests part 3, slice 1 (quest objects): using a body, a crate, a root or a building site of a quest.  Included at the end of session/player_session.cpp.
//
//  The client sends ONE ServerInteractWithInteractableComponent when the player presses the key and shows the use itself (the kneel, the bar): the object's player interaction says how
//  long it takes.  A use that takes time is run here: the server answers ClientOnDoInteract when it ends, as the game's server does (a recorded delivery to a building site answers
//  4.5 seconds after the call, an instant use 68 ms after it).  The prompt appears on the client when the interactable's rule holds (an objective of the quest is active); the server
//  checks the same rule and the reach.  When the use ends: the object answers, gives what its player interaction gives, counts for the Interact objectives that name its definition and, when
//  its actor definition says so, leaves and is made again after its instance's own respawn time (PlayerWorld::prop_gone).
// ============================================================================

void PlayerSession::on_prop_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const std::string label = a.npc.archetype.empty() ? std::string("a quest object") : spawns::blueprint_leaf(a.npc.archetype);
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(prop_logged_, 60), "[props] {}: a use of {} is ignored: {}", name(), label, why);
    };
    const content::ObjectData* objects = world_ ? world_->objects() : nullptr;
    if (!objects) return ignore("the world has no quest objects");
    const auto def = objects->interactables.find(a.prop.component_def);
    if (def == objects->interactables.end()) return ignore("its interactable definition is not in the data");
    const content::InteractableRow& row = def->second;
    if (view_->state_of(a.id) != world::ClientState::Awake) return ignore("the client does not have that object open");
    if (prop_using_) {
        if (prop_actor_ != a.id) return ignore("another use is running");
        return;   // (the same object again: the use runs)
    }
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a.x - at.x, a.y - at.y);
    const double reach = row.radius + 300.0;   // the definition's reach, and the margin a step or a lag takes
    if (distance > reach) return ignore("it is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away (its reach is " + std::to_string(static_cast<int>(row.radius / 100.0)) + " m)");
    if (combat_flag_ && !row.allow_combat) return ignore("the player is in combat");
    if (!row.requirement.empty()) {   // the rule of the prompt: an objective of the quest is active
        const dialogue::Book* book = world_->dialogue();
        if (!book) return ignore("the world has no conversations to read its rule with");
        const auto facts = dialogue_facts(nullptr);
        dialogue::Evaluator ev(*book, *facts);
        if (!ev.holds(row.requirement)) return ignore("its rule does not hold (the objective it belongs to is not active)");
    }
    if (!within_budget(vendor_budget_, "a quest object", now) || !room_for(2)) return;
    if (row.channel > 0.05) {
        prop_using_ = true;
        prop_actor_ = a.id;
        prop_end_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(row.channel));
        prop_x_ = at.x;
        prop_y_ = at.y;
        spdlog::log(level_for(prop_logged_, 60), "[props] {}: starts to use {} ({:.0f} m away): {:.1f} s", name(), label, distance / 100.0, row.channel);
        return;
    }
    finish_prop_use(conn, a.id, now);
}

void PlayerSession::run_props(Connection& conn, Clock::time_point now) {
    if (!prop_using_) return;
    const content::ObjectData* objects = world_ ? world_->objects() : nullptr;
    const world::WorldActor* a = registry_ ? registry_->find(prop_actor_) : nullptr;
    const auto cancel = [&](const char* why) {
        prop_using_ = false;
        ++prop_cancelled_;
        spdlog::log(level_for(prop_logged_, 60), "[props] {}: the use of a quest object stops: {}", name(), why);
    };
    if (!a || a->kind != world::ActorKind::Prop || !objects) return cancel("the object is gone");
    if (dead_) return cancel("the player died");
    const GoodPosition at = checker_.good();
    if (std::hypot(at.x - prop_x_, at.y - prop_y_) > 150.0) return cancel("the player stepped away");
    if (now < prop_end_) return;
    prop_using_ = false;
    finish_prop_use(conn, prop_actor_, now);
}

void PlayerSession::finish_prop_use(Connection& conn, uint64_t actor_id, Clock::time_point now) {
    const world::WorldActor* a = registry_ ? registry_->find(actor_id) : nullptr;
    const content::ObjectData* objects = world_ ? world_->objects() : nullptr;
    if (!a || !objects) return;
    const auto def = objects->interactables.find(a->prop.component_def);
    if (def == objects->interactables.end()) return;
    const content::InteractableRow& row = def->second;
    if (!room_for(24)) return;
    ++prop_uses_;
    const std::string label = spawns::blueprint_leaf(a->npc.archetype);
    // the object answers
    const FIntrepidNetworkGUID component = component_guid_of(*a, a->npc.interact_component);
    const FIntrepidNetworkGUID actor = a->actor.guid;
    queue("ClientOnDoInteract", [component, actor](bootstrap::PlayerBootstrap& pb) { return pb.interact_done(component, actor); }, conn, /*must_build=*/true);
    // what its use gives
    if (!row.grants.empty()) {
        gather::Harvest h;
        h.durability_loss = 0;
        for (const auto& [item, quantity] : row.grants) {
            combat::RolledItem r;
            r.item = item;
            r.quantity = quantity;
            h.items.push_back(r);
        }
        HarvestFrame frame;
        frame.what = std::string("the use of ") + label;
        frame.harvest = &h;
        frame.tag = "quest";
        deliver_harvest(conn, now, frame);
    }
    if (row.reward_table != 0) grant_quest_rewards(conn, {row.reward_table}, "a quest object", now, 0);
    if ((!row.complete_objs.empty() || !row.reveal_objs.empty()) && quest_told_.insert("prop objectives " + label).second) {
        spdlog::info("[props] {}: the use of {} completes or reveals objectives, which this build does not do (told once)", name(), label);
    }
    // the quests it counts for
    quest_engine(conn, now, "a quest object", [&](quest::Engine& eng) { eng.interact(a->prop.component_def, a->prop.actor_def); });
    spdlog::log(level_for(prop_logged_, 60), "[props] {}: used {} ({} item(s) given{})", name(), label, row.grants.size(), row.reward_table != 0 ? ", a reward table" : "");
    // it leaves when its actor definition says so (after the delay it names), and comes back after its instance's respawn time
    const auto ad = a->prop.actor_def != 0 ? objects->actor_defs.find(a->prop.actor_def) : objects->actor_defs.end();
    if (ad != objects->actor_defs.end() && ad->second.despawn_final) {
        if (world_->prop_gone(actor_id, world_->world_clock() + std::max(0.0, ad->second.despawn_delay))) registry_->remove(actor_id);
    }
}
