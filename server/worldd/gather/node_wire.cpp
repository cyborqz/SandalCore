// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  gather/node_wire.cpp (see node_wire.h)
// ============================================================================
#include "gather/node_wire.h"

#include "combat/npc_replicator.h"

#include <algorithm>
#include <cstring>

namespace aoc::worldd::gather {

using repl::Value;

namespace {

repl::Subobject* component(repl::Actor& actor) {
    for (repl::Subobject& s : actor.subobjects) {
        if (s.obj.schema && std::strcmp(s.obj.schema->name, "UInteractableComponent") == 0) return &s;
    }
    return nullptr;
}

const repl::Subobject* component(const repl::Actor& actor) {
    for (const repl::Subobject& s : actor.subobjects) {
        if (s.obj.schema && std::strcmp(s.obj.schema->name, "UInteractableComponent") == 0) return &s;
    }
    return nullptr;
}

bool fail(std::string& error, const char* what) {
    error = what;
    return false;
}

/// The array value of a property as the object holds it now (empty when it holds none); at least four elements (a static array of four inputs).
repl::ArrayValue four_of(const repl::ObjectState& o, const char* path) {
    repl::ArrayValue a;
    if (const repl::Value* v = o.state.get(*o.schema, path)) {
        if (const auto* old = v->get_if<repl::ArrayValue>()) a = *old;
    }
    if (a.elements.size() < 4) a.elements.resize(4);
    return a;
}

Value word(uint32_t w) { return Value::i64(static_cast<int32_t>(w)); }
// [C | Y | B x CoA]

/// InputEffectStatuses[0] with the status, bWasInteractedWith and no InteractingPlayer.
bool write_status(repl::Subobject& inter, uint32_t status, bool interacted, std::string& error) {
    repl::ArrayValue statuses = four_of(inter.obj, "InputEffectStatuses");
    repl::ElementState& e = statuses.elements[kInput];
    e.values[1] = Value::u64(status);
    e.values[2] = Value::boolean(interacted);
    e.values[3] = Value(protocol::emit::FIntrepidNetworkGUID{});
    if (!inter.obj.state.set(*inter.obj.schema, "InputEffectStatuses", Value(std::move(statuses)))) return fail(error, "the node's interaction does not take its status");
    return true;
}

bool write_state(repl::Subobject& inter, uint64_t interaction_state, std::string& error) {
    repl::ArrayValue states = four_of(inter.obj, "CurrentInputStates");
    states.elements[kInput].values[1] = Value::u64(interaction_state);
    if (!inter.obj.state.set(*inter.obj.schema, "CurrentInputStates", Value(std::move(states)))) return fail(error, "the node's interaction does not take its state");
    return true;
}

bool write_channeling(repl::Subobject& inter, const repl::GuidValue* player, std::string& error) {
    repl::ArrayValue guids;
    if (player) {
        repl::ElementState e;
        e.values[1] = word(player->a);
        e.values[2] = word(player->b);
        e.values[3] = word(player->c);
        e.values[4] = word(player->d);
        guids.elements.push_back(std::move(e));
    }
    if (!inter.obj.state.set(*inter.obj.schema, "PlayerGuidsActiveChanneling", Value(std::move(guids)))) return fail(error, "the node's interaction does not take who channels");
    return true;
}

/// CanInputActivate, the four inputs' gate.  The client's UInteractableComponent::CanInputActivate indexes the replicated array without a bound (`index < Num & Data[index]`), so an
/// EMPTY array (a null Data) is an access violation at the next prompt evaluation: Gathering stage 2, test 1 (the Daffodil cancel; node_reset wrote `ArrayValue{}`).  This is the only
/// writer of the property, and it always writes four entries: `first` for the interaction (input 0); the other three true when `others_open` (the allowed form of
/// world::allow_inputs, the GM vendors' open, which this client takes) and left as the client has them otherwise (retail's done update writes the first entry alone, 33 bits).
bool write_can_input(repl::Subobject& inter, bool first, bool others_open, std::string& error) {
    repl::ArrayValue can;
    can.elements.resize(kInputs);
    can.elements[kInput].values[1] = Value::boolean(first);
    if (others_open) {
        for (size_t i = 1; i < can.elements.size(); ++i) can.elements[i].values[1] = Value::boolean(true);
    }
    if (!inter.obj.state.set(*inter.obj.schema, "CanInputActivate", Value(std::move(can)))) return fail(error, "the node's interaction does not take CanInputActivate");
    return true;
}

/// All four inputs' current interaction state (a corpse's done update changes them together; a node's changes the first alone).
bool write_state_all(repl::Subobject& inter, uint64_t interaction_state, std::string& error) {
    repl::ArrayValue states = four_of(inter.obj, "CurrentInputStates");
    for (repl::ElementState& e : states.elements) e.values[1] = Value::u64(interaction_state);
    if (!inter.obj.state.set(*inter.obj.schema, "CurrentInputStates", Value(std::move(states)))) return fail(error, "the corpse's interaction does not take its states");
    return true;
}

bool write_gathered(world::WorldActor& node, bool gathered, std::string& error) {
    repl::ObjectState& actor = node.actor.obj;
    if (!actor.schema || !actor.schema->command("bGathered")) return fail(error, "the actor has no bGathered");
    if (!actor.state.set(*actor.schema, "bGathered", Value::boolean(gathered))) return fail(error, "the actor does not take bGathered");
    return true;
}

}  // namespace

bool node_gathered(const world::WorldActor& node) {
    const repl::ObjectState& actor = node.actor.obj;
    if (!actor.schema || !actor.schema->command("bGathered")) return false;
    const repl::Value* v = actor.state.get(*actor.schema, "bGathered");
    const bool* b = v ? v->get_if<bool>() : nullptr;
    return b && *b;
}

uint32_t node_status(const world::WorldActor& node) {
    const repl::Subobject* inter = component(node.actor);
    if (!inter) return 0;
    const repl::Value* v = inter->obj.state.get(*inter->obj.schema, "InputEffectStatuses");
    const auto* a = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!a || a->elements.size() <= static_cast<size_t>(kInput)) return 0;
    const auto it = a->elements[kInput].values.find(1);
    const uint64_t* s = it == a->elements[kInput].values.end() ? nullptr : it->second.get_if<uint64_t>();
    return s ? static_cast<uint32_t>(*s) : 0;
}

bool node_begin(world::WorldActor& node, const Channeler& by, std::string& error) {
    if (node.kind != world::ActorKind::Gatherable) return fail(error, "the actor is no resource node");
    repl::Subobject* inter = component(node.actor);
    if (!inter) return fail(error, "the node has no interactable component");
    world::WorldActor backup = node;
    if (!write_status(*inter, status_word(0, kPhaseChannel), true, error) || !write_channeling(*inter, &by.character, error)) {
        node = std::move(backup);   // a refused part leaves nothing half done
        return false;
    }
    node.gather.player_id = by.player_id;
    node.gather.pawn = by.pawn;
    node.gather.show = true;
    return true;
}

bool node_beat(world::WorldActor& node, uint32_t step, std::string& error) {
    if (node.kind != world::ActorKind::Gatherable || step == 0) return fail(error, "no heartbeat for this actor or step");
    repl::Subobject* inter = component(node.actor);
    if (!inter) return fail(error, "the node has no interactable component");
    if (!write_status(*inter, status_word(step, kPhaseChannel), true, error)) return false;
    node.gather.show = false;   // the first heartbeat takes InteractingPlayer off the harvester's view [R: 1427 packet 73]
    return true;
}

bool node_done(world::WorldActor& node, uint32_t step, std::string& error) {
    if (node.kind != world::ActorKind::Gatherable || step == 0) return fail(error, "no done update for this actor or step");
    repl::Subobject* inter = component(node.actor);
    if (!inter) return fail(error, "the node has no interactable component");
    world::WorldActor backup = node;
    // CanInputActivate: four entries of which only the first is written (false) [R: 1427 packet 674: 33 bits]
    if (!write_gathered(node, true, error) || !write_status(*inter, status_word(step, kPhaseDone), true, error) || !write_channeling(*inter, nullptr, error) ||
        !write_can_input(*inter, /*first=*/false, /*others_open=*/false, error) || !write_state(*inter, 0, error)) {
        if (error.empty()) error = "the node's interaction does not take its done state";
        node = std::move(backup);
        return false;
    }
    node.gather.show = true;   // the harvester's own view has its pawn in the done update [R: 1427 packet 674]
    return true;
}
// CoA.C | Y | B

bool node_reset(world::WorldActor& node, std::string& error) {
    if (node.kind != world::ActorKind::Gatherable) return fail(error, "the actor is no resource node");
    repl::Subobject* inter = component(node.actor);
    if (!inter) return fail(error, "the node has no interactable component");
    world::WorldActor backup = node;
    // CanInputActivate: the four inputs allowed, never an empty array (the client crashes on one: write_can_input)
    if (!write_gathered(node, false, error) || !write_status(*inter, 0, false, error) || !write_channeling(*inter, nullptr, error) ||
        !write_can_input(*inter, /*first=*/true, /*others_open=*/true, error) || !write_state(*inter, 1, error)) {
        if (error.empty()) error = "the node's interaction does not take its reset";
        node = std::move(backup);
        return false;
    }
    node.gather = world::GatherSeen{};
    return true;
}

// ---- Hunting: a huntable creature's corpse ----------------------------------------------------------------------------------------------------------------------------------------

bool corpse_begin(world::WorldActor& corpse, const Channeler& by, std::string& error) {
    if (corpse.kind != world::ActorKind::Npc) return fail(error, "the actor is no NPC");
    repl::Subobject* inter = component(corpse.actor);
    if (!inter) return fail(error, "the creature has no interactable component");
    world::WorldActor backup = corpse;
    if (!write_status(*inter, corpse_status_word(1, kPhaseChannel), true, error) || !write_channeling(*inter, &by.character, error)) {
        corpse = std::move(backup);
        return false;
    }
    corpse.gather.player_id = by.player_id;
    corpse.gather.pawn = by.pawn;
    corpse.gather.show = true;
    return true;
}

bool corpse_beat(world::WorldActor& corpse, uint32_t counter, std::string& error) {
    if (corpse.kind != world::ActorKind::Npc) return fail(error, "the actor is no NPC");
    repl::Subobject* inter = component(corpse.actor);
    if (!inter) return fail(error, "the creature has no interactable component");
    if (!write_status(*inter, corpse_status_word(counter, kPhaseChannel), true, error)) return false;
    corpse.gather.show = false;   // the first heartbeat takes InteractingPlayer off the harvester's view [R: 1457 packet 139]
    return true;
}

bool corpse_done(world::WorldActor& corpse, uint32_t counter, std::string& error) {
    if (corpse.kind != world::ActorKind::Npc) return fail(error, "the actor is no NPC");
    repl::Subobject* inter = component(corpse.actor);
    if (!inter) return fail(error, "the creature has no interactable component");
    world::WorldActor backup = corpse;
    // [R: 1457 packet 995] the status (phase 4), nobody channeling, CanInputActivate [false] (the first entry alone), the four states 0; the corpse is dead for its clients
    if (!write_status(*inter, corpse_status_word(counter, kPhaseDone), true, error) || !write_channeling(*inter, nullptr, error) || !write_can_input(*inter, /*first=*/false, /*others_open=*/false, error) ||
        !write_state_all(*inter, 0, error)) {
        if (error.empty()) error = "the corpse's interaction does not take its done state";
        corpse = std::move(backup);
        return false;
    }
    combat::mark_dead(corpse);
    corpse.gather.show = true;   // the harvester's own view has its pawn in the done update [R: 1457 packet 995]
    return true;
}

bool corpse_reset(world::WorldActor& corpse, std::string& error) {
    if (corpse.kind != world::ActorKind::Npc) return fail(error, "the actor is no NPC");
    repl::Subobject* inter = component(corpse.actor);
    if (!inter) return fail(error, "the creature has no interactable component");
    world::WorldActor backup = corpse;
    // CanInputActivate: the four inputs allowed, never an empty array (the client crashes on one: write_can_input); the living huntable's states were all 1
    if (!write_status(*inter, 0, false, error) || !write_channeling(*inter, nullptr, error) || !write_can_input(*inter, /*first=*/true, /*others_open=*/true, error) || !write_state_all(*inter, 1, error)) {
        if (error.empty()) error = "the corpse's interaction does not take its reset";
        corpse = std::move(backup);
        return false;
    }
    corpse.gather = world::GatherSeen{};
    return true;
}

bool show_interacting_player(const world::WorldActor& node, uint64_t player_id, repl::Actor& actor) {
    if ((node.kind != world::ActorKind::Gatherable && node.kind != world::ActorKind::Npc) || !node.gather.show || node.gather.player_id == 0 || node.gather.player_id != player_id || node.gather.pawn.ObjectId == 0) return false;
    repl::Subobject* inter = component(actor);
    if (!inter) return false;
    repl::ArrayValue statuses = four_of(inter->obj, "InputEffectStatuses");
    statuses.elements[kInput].values[3] = Value(node.gather.pawn);
    return inter->obj.state.set(*inter->obj.schema, "InputEffectStatuses", Value(std::move(statuses)));
}

}  // namespace aoc::worldd::gather
