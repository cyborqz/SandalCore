// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_items.cpp
//
//  Items in the bags and the storages: applying the outcome of a request, the resource bags, refused item requests, using an item from a bag, Glint tiers, storage events and payloads,
//  the crate on the back, the item toasts and the game master item grant.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "combat/combat_world.h"
#include "crafting/job.h"
#include "crafting/rewards.h"
#include "crafting/rules.h"
#include "crafting/station.h"
#include "content/content_db.h"
#include "dialogue/talk.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "world/city_nodes.h"
#include "protocol/repl/hit_rpc.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/storage_rpc.h"
#include "session/gm_travel.h"
#include "protocol/repl/rpc_fields.h"
#include "items/acquired.h"
#include "vendor/node_tax.h"
#include "vendor/sell_rules.h"
#include "quest/quest_wire.h"
#include "quest/registry_points.h"
#include "world/zoi_surface.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>

#include "session/player_session_util.h"

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;
namespace schema = ::aoc::protocol::schema;
namespace wire = ::aoc::protocol::wire;
using bootstrap::Role;
using protocol::emit::FIntrepidNetworkGUID;

/// ServerActivateItem: not answered; the client sends it with every ServerItemUsed of an item that can be worn, with the same parameters.
void PlayerSession::on_activate_item_ignored() {
    // the retail client sends it with every ServerItemUsed of an item that can be worn, with the same parameters; the answer to the
    // pair is one set of events, made for ServerItemUsed
    if (!activate_logged_) {
        activate_logged_ = true;
        spdlog::debug("[session] {}: ServerActivateItem is not answered: ServerItemUsed carries the same request", name());
    }
    return;
}
// (C-Y-B ~ CoA)

// ============================================================================================================================
//  items (plan B2)
// ============================================================================================================================

std::vector<bootstrap::ItemEvent> item_events_for(const items::Outcome& outcome) {
    std::vector<bootstrap::ItemEvent> out;
    out.reserve(outcome.events.size());
    for (const items::Event& e : outcome.events) {
        bootstrap::ItemEvent ev;
        ev.type = e.type;
        ev.storage = bootstrap::item_storage_of(e.container);
        ev.slot = e.slot;
        ev.item = items::to_instance(e.item);
        ev.instigated = e.instigated;
        ev.count = e.count;
        out.push_back(std::move(ev));
    }
    return out;
}
// ~ C Y B - C-O-A ~

namespace {

/// A catalog that knows no record: a session without one routes every grant to the Backpack.
struct NoCatalog : items::ItemCatalog {
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

/// The material bag (Resource0 to Resource2) an item location names: the places a Glint stack is used in.
std::optional<svc::ItemContainer> material_bag_of_location(uint8_t location) {
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (location == static_cast<uint8_t>(repl::ItemLocation::Resource0) + i) return items::resource_container(i);
    }
    return std::nullopt;
}

}  // namespace

const items::ItemCatalog& PlayerSession::item_catalog() const {
    static const NoCatalog none;
    return catalog_ ? *catalog_ : none;
}

std::string PlayerSession::fresh_guid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    for (;;) {
        const uint64_t a = rng(), b = rng();
        const repl::GuidValue g{static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a), static_cast<uint32_t>(b >> 32), static_cast<uint32_t>(b)};
        if (!(g.a | g.b | g.c | g.d)) continue;
        const std::string hex = items::guid_hex(g);
        bool taken = false;
        for (const svc::InventoryItem& it : inventory_.items()) taken = taken || it.instance_guid == hex;
        if (!taken) return hex;
    }
}

void PlayerSession::apply_outcome(Connection& conn, const items::Outcome& outcome, const char* what, bool with_toasts) {
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    if (outcome.equipment_changed && book_ && world_ && world_->content()) {
        // plan D1, stage 2: the weapon line in hand follows what is worn (a weapon tree's passives apply while its line is in hand).  The weapon swap fix: so do the weapon's combo, the
        // ability lists, OwnerInfo's line and the ranged flag; they are staged in the pawn here and ride in the chain of the HeldItems step queued below (it diffs the whole pawn)
        for (const items::Event& e : outcome.events) {
            if (e.container == svc::ItemContainer::Equipment && (e.slot == items::equip::Primary || e.slot == items::equip::Ranged)) {
                follow_worn_weapon();
                break;
            }
        }
    }
    if (outcome.equipment_changed) apply_gear_stats();   // before the replies are queued: they are built later, and carry the stats' deltas
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    if (with_toasts) add_item_toasts(events, /*removals=*/true);
    // the storage events go first (the PlayerController's chain), then the pawn's visible gear: the order and the packet of the retail answer
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    if (outcome.equipment_changed) {
        const std::vector<bootstrap::HeldItem> held = bootstrap::held_items_for(inventory_);
        const std::vector<repl::AppearanceId> appearance = items::appearance_ids(inventory_);
        // the retail server marks the 23 held items once for each change of the Equipment storage its answer holds
        size_t marks = 0;
        for (const items::Event& e : outcome.events) marks += e.container == svc::ItemContainer::Equipment ? 1 : 0;
        queue("HeldItems and AppearanceIDs",
              [held, appearance, marks](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
                  if (!pb.apply_equipment(held, appearance, marks ? marks : 1)) return std::nullopt;
                  return pb.equipment_update();
              },
              conn, /*must_build=*/true);
    }
    spdlog::log(level_for(item_applied_logged_), "[session] {}: {}: {} storage event(s){}", name(), what, outcome.events.size(),
                outcome.equipment_changed ? ", the visible gear is updated" : "");
    note_cargo_crate(outcome);
    if (outcome.equipment_changed) sync_cargo(conn, last_now_);
    // a resource bag worn, replaced or taken off: its container opens or closes on the client now, after the events that moved the bag
    for (const items::BagChange& c : outcome.bags) update_resource_container(conn, c);
}

void PlayerSession::note_cargo_crate(const items::Outcome& outcome) {
    for (const items::Event& e : outcome.events) {
        if (e.type != repl::StorageEventType::Add || e.container != svc::ItemContainer::Equipment || e.slot != items::equip::CargoHold) continue;
        const content::ContentDb* db = world_ ? world_->content() : nullptr;
        std::string error;
        const auto gear = db ? db->item_gear(e.item.item_record_id, error) : std::nullopt;
        if (!gear) continue;
        for (const std::string& tag : gear->tags) {
            std::string lower;
            for (const char c : tag) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "item.crate.construction") {
                construction_crate_equipped(e.item.item_record_id);
                break;
            }
        }
    }
}

void PlayerSession::sync_cargo(Connection& conn, Clock::time_point when) {
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if (!crate && cargo_effects_.empty()) return;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !phase_in_done_) return;
    // the effects the crate's record lists for the rarity the crate shows (item_equipped_effect: the Encumbered effect lists Common alone, so a crate of a better rarity slows by its
    // stat block and shows no icon: the original's craft of an Uncommon crate puts no effect on the pawn)
    std::map<int64_t, int> wanted;
    if (crate) {
        std::string error;
        const auto listed = db->item_equipped_effects(crate->item_record_id, error);
        if (!listed) spdlog::warn("[session] {}: the effects of the crate cannot be read: {}", name(), error);
        const int rarity = world_->item_rarity_facts(crate->item_record_id).rarity_of(crate->quality);
        for (const content::EquippedEffect& e : listed ? *listed : std::vector<content::EquippedEffect>{}) {
            if (rarity >= e.min_rarity && rarity <= e.max_rarity) wanted[e.effect] = e.stacks;
        }
    }
    bootstrap::AbilityChange change;
    std::vector<std::pair<int64_t, int64_t>> removed, added;   // (effect record, key)
    for (auto it = cargo_effects_.begin(); it != cargo_effects_.end();) {
        if (wanted.count(it->first)) {
            ++it;
            continue;
        }
        change.effects_removed.push_back(it->second);
        removed.emplace_back(it->first, it->second);
        it = cargo_effects_.erase(it);
    }
    for (const auto& [effect, stacks] : wanted) {
        if (cargo_effects_.count(effect)) continue;
        bootstrap::EffectItem e;
        e.effect_id = next_passive_effect_id_++;
        e.effect_guid = effect;
        e.start_time = bootstrap::effect_clock(pb_.setup().world_time + session_seconds(when));
        e.duration = std::numeric_limits<float>::max();   // (the effects an item lists are infinite: they last while the item is worn)
        e.tick_time = 0;
        e.stacks = stacks;
        change.effects_added.push_back(e);
        added.emplace_back(effect, e.effect_id);
        cargo_effects_[effect] = e.effect_id;
    }
    if (!change.empty()) {
        if (pb_.apply_ability_change(change)) {
            queue_later("pawn state", [](bootstrap::PlayerBootstrap& pb) { return pb.pawn_state_update("the crate"); }, conn, when);
            spdlog::info("[session] {}: the crate on the back {}: {} effect item(s) added, {} removed", name(), crate ? "puts its effects on the pawn" : "is gone", added.size(), removed.size());
        } else {
            spdlog::warn("[session] {}: the pawn does not take the crate's effect items ({}): the crate is carried without them", name(),
                         pb_.error().empty() ? "keys disagree" : pb_.error());
            for (const auto& a : added) cargo_effects_.erase(a.first);
            for (const auto& r : removed) cargo_effects_[r.first] = r.second;
        }
    }
    // Drop Crate goes on the bar when a crate is carried and the ability is on no slot yet (the original sends a ClientReceiveActionbarSlotPayload with bank 2, slot 11 and bAutoGranted around
    // the crate's Add, and none in a craft that followed, with the ability on the bar already).  A slot someone else holds is not taken: the first free class slot then.
    if (crate && book_ && std::find(known_.begin(), known_.end(), content::kGuidDropCrate) != known_.end()) {
        bool on_bar = false;
        for (const repl::ActionbarSlot& s : bar_.slots()) on_bar = on_bar || (s.use.use_type == repl::ActionbarUseType::Ability && s.use.ability_id == content::kGuidDropCrate);
        if (!on_bar) {
            repl::ActionbarSlot slot;
            slot.bank_index = kDropCrateBank;
            slot.slot_index = kDropCrateSlot;
            slot.use.use_type = repl::ActionbarUseType::Ability;
            slot.use.ability_id = content::kGuidDropCrate;
            if (bar_.at(slot.bank_index, slot.slot_index)) {
                const auto spare = bar_.first_free();
                if (spare) {
                    slot.bank_index = spare->first;
                    slot.slot_index = spare->second;
                }
            }
            std::string why;
            if (!bar_.at(slot.bank_index, slot.slot_index) && bar_.set(slot, &why)) {
                repl::ActionbarPayload payload;
                payload.slots.push_back(slot);
                payload.auto_granted = true;
                queue_later("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn, when, /*must_build=*/true);
                progress_changed_ = true;
                hand_over_progress("the action bar");
                spdlog::info("[session] {}: Drop Crate is put on the action bar (bank {}, slot {}): a crate is carried", name(), slot.bank_index, slot.slot_index);
            }
        }
    }
}

void PlayerSession::run_cargo(Connection& conn, Clock::time_point now) {
    if (finished_ || failed_ || !phase_in_done_ || !join_.empty()) return;
    if (!cargo_synced_ && room_for(4)) {
        cargo_synced_ = true;
        sync_cargo(conn, now);
    }
    if (crate_drop_due_ && now >= *crate_drop_due_) {
        crate_drop_due_.reset();
        drop_crate(conn, now);
    }
}

void PlayerSession::drop_crate(Connection& conn, Clock::time_point now) {
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if (!crate || dead_ || !room_for(8)) return;
    const GoodPosition at = checker_.good();
    if (mounted() || at.movement_mode != repl::kMoveWalking) {   // [M] retail refuses a drop at a place the crate cannot lie, with no event: the ground under the character is the model
        spdlog::info("[session] {}: General_DropCrate: the crate stays on the back ({})", name(), mounted() ? "the character is mounted" : "the character is not on the ground");
        return;
    }
    const std::string guid = crate->instance_guid;
    items::Outcome o = inventory_.use(svc::ItemContainer::Equipment, items::equip::CargoHold, guid, 0xFFFFFFFFu, item_catalog());
    if (!o.ok) {
        say(conn, "There is no room in your bags for the crate.", now);
        spdlog::info("[session] {}: General_DropCrate: the crate stays on the back: {}", name(), o.refusal);
        return;
    }
    apply_outcome(conn, o, "a crate was dropped");
    spdlog::info("[session] {}: General_DropCrate: the crate left the back (into the item bag: the ground crates are not built yet)", name());
}

void PlayerSession::update_resource_container(Connection& conn, const items::BagChange& change) {
    if (change.index < 0 || change.index >= items::kResourceContainers) return;
    const size_t i = static_cast<size_t>(change.index);
    const items::ResourceGrid& grid = inventory_.grid(items::resource_container(change.index));
    bootstrap::ResourceBagSetup bag;
    FIntrepidNetworkGUID guid = pb_.setup().guids.resource_storage[i];
    if (change.open && !grid.empty()) {
        bag.present = true;
        bag.columns = grid.columns();
        bag.rows = grid.rows();
        bag.override_matrix = grid.override_matrix();
        if (world_) guid = world_->mint_guid();   // a container that opens is a new component with a guid of its own
    }
    spdlog::info("[session] {}: the resource bag in Equipment slot {} {}: container Resource{} {}", name(), items::equip::ResourceBag0 + change.index,
                 bag.present ? "is worn" : "is taken off", change.index, bag.present ? std::to_string(bag.columns) + " x " + std::to_string(bag.rows) + " opens now" : std::string("closes now"));
    const int index = change.index;
    queue("the resource containers", [index, bag, guid](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
        if (!pb.set_resource_container(index, bag, guid)) return std::nullopt;
        return pb.resource_containers_update();
    }, conn, /*must_build=*/true);
    if (bag.present) {
        // the server hands the grid to the client with the container (the client asks for it too, as a client does for any container it learns of: the answer is the same)
        const bootstrap::StorageKind kind = bootstrap::resource_kind(index);
        queue("ClientReceiveStoragePayload", [kind](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(kind, Role::Pawn); }, conn);
    }
}
// (C Y B | CoA)

void PlayerSession::refuse_item_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now) {
    ++items_refused_;
    spdlog::log(level_for(item_refused_logged_), "[session] {}: {} refused, nothing changed: {}", name(), what, why);
    // the client is told what is where, as at the join; a client that is refused over and over gets that a few times, then silence
    if (!resync_budget_.take(now) || !room_for(5)) return;
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));   // Gathering stage 3: a refused drag in a resource container too
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
    }
}

void PlayerSession::on_item_used(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto use = repl::parse_item_used(rpc.call);
    if (!use) {
        if (!item_unreadable_logged_) {
            item_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerItemUsed does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(item_budget_, "ServerItemUsed", now) || !room_for(8)) return;
    if (!catalog_) return refuse_item_request(conn, "ServerItemUsed", "worldd has no item catalog", now);
    const auto storage = container_of_location(use->storage);
    if (!storage) {
        if (const auto bag = material_bag_of_location(use->storage)) return on_tier_up(conn, *use, *bag, now);   // Glint tiers: a stack used in a material bag
        return refuse_item_request(conn, "ServerItemUsed", "items of storage " + std::to_string(use->storage) + " are not used", now);
    }
    // the mounts milestone: a mount item is neither worn nor consumed; the summon or the desummon is the packet's next call
    if (mounts_ && *storage == svc::ItemContainer::Backpack) {
        const svc::InventoryItem* item = inventory_.at(*storage, use->slot);
        if (item && item->instance_guid == items::guid_hex(use->expected_item) && mounts_->is_mount_item(item->item_record_id)) {
            const svc::InventoryItem copy = *item;
            return on_mount_item_used(copy);
        }
    }
    // plan D1: a bag item that content.db lists no slot for is used, not worn (the item must be where the client says, as for an equip)
    if (*storage == svc::ItemContainer::Backpack) {
        const svc::InventoryItem* item = inventory_.at(*storage, use->slot);
        if (item && item->instance_guid == items::guid_hex(use->expected_item)) {
            const auto info = catalog_->info(item->item_record_id);
            if (!info || !info->wearable()) {
                // a consumable casts an ability: the client sends the ability's trigger in the same datagram and the cast runs the use (nothing is done here)
                if (item_casts_ability(item->item_record_id)) return;
                const svc::InventoryItem copy = *item;   // the inventory changes under the use
                // chests: a random coffer rolls its table, a selector opens its reward window; neither is a generic item action
                if (open_chest(conn, *use, copy, now)) return;
                return on_item_activated(conn, *use, copy, now);
            }
        }
    }
    const items::Outcome outcome = inventory_.use(*storage, use->slot, items::guid_hex(use->expected_item), use->requested_equip_slot, *catalog_);
    if (!outcome.ok) {
        // a resource bag that still holds items stays where it is; the player is told why nothing happened
        if (outcome.holds_items) send_economy_callout(conn, "CantUnequipResourceBag", now);
        return refuse_item_request(conn, "ServerItemUsed", outcome.refusal, now);
    }
    apply_outcome(conn, outcome, *storage == svc::ItemContainer::Backpack ? "an item is worn" : "an item is taken off");
}

void PlayerSession::on_tier_up(Connection& conn, const repl::ItemUse& use, svc::ItemContainer container, Clock::time_point now) {
    const char* what = "ServerItemUsed (a Glint tier change)";
    const auto refuse = [&](const std::string& why) {
        ++uses_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const char* bag = items::container_name(container);
    const svc::InventoryItem* held = inventory_.at(container, use.slot);
    if (!held) return refuse(std::string("no item in ") + bag + " slot " + std::to_string(use.slot));
    if (held->instance_guid != items::guid_hex(use.expected_item)) return refuse(std::string("the item in ") + bag + " slot " + std::to_string(use.slot) + " is not the instance the client named");
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db) return refuse("worldd has no content.db to read the item's use from");
    std::string error;
    const auto activation = db->item_use(held->item_record_id, error);
    if (!error.empty()) return refuse("content.db failed: " + error);
    // only a stack of an item that turns into the next kind is used in a material bag; the other uses (a crate opened from a bag) are not made
    if (!activation || activation->activation != "TierUpItem") return refuse(std::string("items of the ") + bag + " are used only to change a Glint stack into the next kind");
    const items::CurrencyTable* currencies = world_->currencies();
    if (!currencies) return refuse("worldd has no currency tiers (rebuild content.db with this checkout's contentc): no stack changes kind");
    const items::CurrencyKind* kind = currencies->kind_of(held->item_record_id);
    const items::CurrencyKind* next = currencies->next_of(held->item_record_id);
    if (!kind) return refuse("item record " + std::to_string(held->item_record_id) + " is no kind of a currency");
    if (!next) return refuse("item record " + std::to_string(held->item_record_id) + " is the top kind of its currency: there is no next kind");
    const auto product_record = db->item(next->item, error);
    if (!product_record) return refuse(error.empty() ? "the next kind, item record " + std::to_string(next->item) + ", is not in content.db" : "content.db failed: " + error);
    const auto stack_record = db->item(held->item_record_id, error);
    // the new item: ONE of the next kind at its record's own quality (21 for a Dim Glint, 41 above), a fresh instance
    svc::InventoryItem product;
    product.container = container;
    product.item_record_id = next->item;
    product.instance_guid = fresh_guid();
    product.quantity = 1;
    product.quality = std::max(1, product_record->default_quality);
    product.durability = product_record->max_durability_at(product.quality);
    product.created_at_ms = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    const svc::InventoryItem stack = *held;   // the inventory changes under the change
    const items::Outcome outcome = inventory_.tier_up(container, use.slot, stack.instance_guid, static_cast<int32_t>(kind->rate), product, *product_record, item_catalog());
    if (!outcome.ok) return refuse(outcome.refusal);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++uses_applied_;
    ++items_applied_;
    // the whole stack goes with no toast of its own; the one new item is told as it arrives, with the "Acquired" toast of the item feed
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/false);
    queue("ClientReceiveStorageEvent", [events](bootstrap::PlayerBootstrap& pb) { return pb.item_events(events); }, conn, /*must_build=*/true);
    const items::Event& arrival = outcome.events.back();
    const std::string from_name = stack_record ? (!stack_record->display_name.empty() ? stack_record->display_name : stack_record->name) : std::to_string(stack.item_record_id);
    const std::string to_name = !product_record->display_name.empty() ? product_record->display_name : product_record->name;
    spdlog::log(level_for(use_logged_), "[session] {}: a Glint tier change: {} x{} in {} slot {} became {} x1 in {} slot {}, {} storage event(s)", name(), from_name, stack.quantity, bag, use.slot,
                to_name, items::container_name(arrival.container), arrival.slot, outcome.events.size());
}

void PlayerSession::on_storage_event_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto event = repl::parse_storage_event(rpc.call);
    if (!event) {
        if (!item_unreadable_logged_) {
            item_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerRequestStorageEvent does not read ({} parameter bits): ignored (further ones are not logged)", name(),
                         rpc.call.param_bits);
        }
        return;
    }
    if (warehouse_event(conn, *event, now)) return;   // storage clerks, step 0: a move that names a warehouse container is the warehouse's
    if (!within_budget(item_budget_, "ServerRequestStorageEvent", now) || !room_for(4)) return;
    const char* what = "ServerRequestStorageEvent";
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    const auto container_of = [&](const protocol::emit::FIntrepidNetworkGUID& object) -> std::optional<svc::ItemContainer> {
        if (object == g.backpack) return svc::ItemContainer::Backpack;
        if (object == g.equipment) return svc::ItemContainer::Equipment;
        if (object == g.quest_storage) return svc::ItemContainer::Quest;
        for (int i = 0; i < items::kResourceContainers; ++i) {
            if (pb_.setup().resource_bags[static_cast<size_t>(i)].present && object == g.resource_storage[static_cast<size_t>(i)]) return items::resource_container(i);
        }
        return std::nullopt;
    };
    const auto type = static_cast<repl::StorageEventType>(event->type);
    if (type == repl::StorageEventType::Remove) {
        // The client destroys an item by dragging it out of a bag (after it asked the player to confirm): the request is the Remove event of the stack, naming the bag, the slot
        // and the instance.  Only the item bag and the material bags are destroyed from; the answer is the same event with the "Removed" toast.
        if (event->targets.size() != 1 || event->targets[0].slots.size() != 1) {
            return refuse_item_request(conn, what, "a destroy names one container and one slot", now);
        }
        const auto where = container_of(event->targets[0].object);
        if (!where) return refuse_item_request(conn, what, "a container that is none of this character's storages", now);
        if (!is_bag(*where)) return refuse_item_request(conn, what, "only an item in a bag is destroyed (not a worn one, not a quest item)", now);
        const repl::StorageEventSlot& slot = event->targets[0].slots[0];
        if (!(slot.item_guid == event->item.guid)) return refuse_item_request(conn, what, "the item of the event is not the instance of its slot", now);
        const std::string guid = items::guid_hex(slot.item_guid);
        const svc::InventoryItem* held = inventory_.at(*where, slot.slot);
        if (!held || held->instance_guid != guid) return refuse_item_request(conn, what, "the slot does not hold that item", now);
        const svc::InventoryItem before = *held;
        const items::Outcome outcome = inventory_.consume(*where, slot.slot, guid, slot.count > 0 ? slot.count : before.quantity);
        if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
        inventory_dirty_ = true;
        inventory_changed_ = true;
        ++items_applied_;
        ++items_destroyed_;
        std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
        add_item_toasts(events, /*removals=*/true);
        for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
            const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                         events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
        spdlog::log(level_for(item_applied_logged_), "[session] {}: an item is destroyed: {} x{} from {} slot {}", name(), before.item_record_id, before.quantity,
                    *where == svc::ItemContainer::Backpack ? "the item bag" : "a material bag", slot.slot);
        return;
    }
    if (type != repl::StorageEventType::Move && type != repl::StorageEventType::Swap) {
        // (a Fill, dragging a stack onto another: Gathering stage 3 refuses it; the backlog has it)
        return refuse_item_request(conn, what, "an event of type " + std::to_string(event->type) + " is not made (only a Move and a Swap of whole stacks, and the destroy)", now);
    }
    if (event->move_source.slots.size() != 1 || event->targets.size() != 1 || event->targets[0].slots.size() != 1) {
        return refuse_item_request(conn, what, "a move names one source slot and one target slot", now);
    }
    const auto from = container_of(event->move_source.object);
    const auto to = container_of(event->targets[0].object);
    if (!from || !to) return refuse_item_request(conn, what, "a container that is none of this character's storages", now);
    const repl::StorageEventSlot& src = event->move_source.slots[0];
    const repl::StorageEventSlot& dst = event->targets[0].slots[0];
    if (!(src.item_guid == event->item.guid)) return refuse_item_request(conn, what, "the item of the event is not the instance of its source slot", now);
    const std::string guid = items::guid_hex(src.item_guid);
    const items::Outcome outcome = type == repl::StorageEventType::Move
                                       ? inventory_.move(*from, src.slot, src.count, guid, *to, dst.slot, &item_catalog())
                                       : inventory_.swap(*from, src.slot, src.count, guid, *to, dst.slot, items::guid_hex(dst.item_guid), dst.count);
    if (!outcome.ok) return refuse_item_request(conn, what, outcome.refusal, now);
    apply_outcome(conn, outcome, type == repl::StorageEventType::Move ? "an item is moved in the bag" : "two items swap places in the bag");
}

void PlayerSession::on_storage_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto it = rpc.call.params.values.find(1);
    const auto* container = rpc.call.decoded && it != rpc.call.params.values.end() ? it->second.get_if<FIntrepidNetworkGUID>() : nullptr;
    const bootstrap::SessionGuids& g = pb_.setup().guids;
    if (container && warehouse_payload(conn, *container, now)) return;   // storage clerks, step 0: a warehouse container's data
    std::optional<bootstrap::StorageKind> kind;
    if (container) {
        if (*container == g.backpack) kind = bootstrap::StorageKind::Backpack;
        else if (*container == g.equipment) kind = bootstrap::StorageKind::Equipment;
        else if (*container == g.quest_storage) kind = bootstrap::StorageKind::QuestStorage;
        else {
            for (int i = 0; i < items::kResourceContainers; ++i) {
                if (pb_.setup().resource_bags[static_cast<size_t>(i)].present && *container == g.resource_storage[static_cast<size_t>(i)]) kind = bootstrap::resource_kind(i);
            }
        }
    }
    if (!kind) {
        spdlog::debug("[session] {}: ServerRequestStorageData names a container worldd has no payload for", name());
        return;
    }
    if (!within_budget(storage_budget_, "ServerRequestStorageData", now) || !room_for(1)) return;
    const bootstrap::StorageKind k = *kind;
    queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::Pawn); }, conn);
}

void PlayerSession::add_item_toasts(std::vector<bootstrap::ItemEvent>& events, bool removals) const {
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    std::map<int64_t, std::string> names;
    std::map<int64_t, items::ItemRarityFacts> rarities;   // the rarity each toast's colour comes from: the record's quality curve held inside its RarityMin..RarityMax
    const auto name_of = [&](int64_t record) -> const std::string& {
        const auto known = names.find(record);
        if (known != names.end()) return known->second;
        std::string shown = "Unknown item";
        std::string error;
        items::ItemRarityFacts facts = items::default_rarity_facts();
        if (db) {
            if (const auto rec = db->item(record, error)) {
                shown = !rec->display_name.empty() ? rec->display_name : rec->name;
                facts = *rec;
            }
        }
        rarities.emplace(record, facts);
        return names.emplace(record, std::move(shown)).first->second;
    };
    const int64_t made = items::ticks_of_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto same_slot = [](const bootstrap::ItemEvent& a, const bootstrap::ItemEvent& b) { return a.storage == b.storage && a.slot == b.slot; };
    for (size_t i = 0; i < events.size(); ++i) {
        bootstrap::ItemEvent& ev = events[i];
        if (!ev.item.has_item) continue;
        if (ev.type == repl::StorageEventType::Add) {
            // a stack that was topped up arrives as the Remove of the old stack and the Add of the new total: the toast says what was added
            repl::ItemInstance shown = ev.item;
            if (i > 0 && events[i - 1].type == repl::StorageEventType::Remove && same_slot(events[i - 1], ev) && events[i - 1].item.has_item &&
                events[i - 1].item.quantity < ev.item.quantity) {
                shown.quantity = ev.item.quantity - events[i - 1].item.quantity;
            }
            const std::string& shown_name = name_of(static_cast<int64_t>(shown.record_id));
            ev.callout = items::acquired_callout(shown, shown_name, rarities.at(static_cast<int64_t>(shown.record_id)), made);
        } else if (removals && ev.type == repl::StorageEventType::Remove) {
            const bool restacked = i + 1 < events.size() && events[i + 1].type == repl::StorageEventType::Add && same_slot(events[i + 1], ev);
            if (!restacked) {
                const std::string& removed_name = name_of(static_cast<int64_t>(ev.item.record_id));
                ev.callout = items::removed_callout(ev.item, removed_name, rarities.at(static_cast<int64_t>(ev.item.record_id)), made);
            }
        }
    }
}

PlayerSession::GmItemResult PlayerSession::gm_give_item(Connection& conn, int64_t item_record, int32_t count, Clock::time_point now, const std::string& by, const std::string& rarity,
                                                         int32_t quality) {
    (void)now;
    GmItemResult r;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db) {
        r.refusal = "worldd has no content.db to read the item from";
        return r;
    }
    if (count < 1 || count > 100000) {
        r.refusal = "the count must be 1 to 100,000";
        return r;
    }
    std::string error;
    const auto rec = db->item(item_record, error);
    if (!rec) {
        r.refusal = error.empty() ? "the data holds no item record " + std::to_string(item_record) : "content.db failed: " + error;
        return r;
    }
    r.item_name = rec->name;
    // item rarities, step 0: the rarity or the quality asked for; an item that can never show it is refused, and the answer lists what it can be
    int32_t given_quality = 0;   // 0: the record's default quality
    if (!rarity.empty() || quality != 0) {
        r.bad_request = true;
        if (!rec->has_quality) {
            r.refusal = rec->name + " has no quality in the game's data (its rarity is fixed by the record): give it without a rarity or a quality";
            return r;
        }
        std::string why;
        given_quality = items::quality_for_request(*rec, rec->name, rarity, quality, why);
        if (given_quality == 0) {
            r.refusal = why;
            return r;
        }
        r.bad_request = false;
    }
    r.quality = given_quality > 0 ? given_quality : rec->default_quality;
    r.rarity = std::string(items::rarity_name(rec->rarity_of(static_cast<double>(r.quality))));
    if (!room_for(2 + static_cast<size_t>(count / std::max(1, rec->max_stack_size)) / bootstrap::kMaxEventsPerChain)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    std::vector<svc::InventoryItem> fresh;
    stacks_of(item_record, count, *rec, fresh, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000, given_quality);
    for (svc::InventoryItem& it : fresh) {
        it.instance_guid = fresh_guid();
        for (const svc::InventoryItem& other : fresh) {
            if (&other != &it && other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
    }
    // Crates: a crate goes where the game puts one that is made or picked up, the Equipment's cargo slot (a second crate, or several, go to the bag as any item does)
    if (const auto crate = item_catalog().info(item_record); crate && crate->cargo_crate() && count == 1 && fresh.size() == 1 && !inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        items::Outcome put = inventory_.grant_cargo(fresh.front(), item_catalog());
        if (!put.ok) {
            r.refusal = "the cargo slot does not take it: " + put.refusal;
            return r;
        }
        r.ok = true;
        r.count = 1;
        r.container = items::container_name(svc::ItemContainer::Equipment);
        apply_outcome(conn, put, "a crate was put in the cargo slot");   // the Add, the worn stats and the visible gear
        spdlog::info("[gm] {}: {} given by {} into the cargo slot (quality {})", name(), r.item_name, by, r.quality);
        return r;
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok) {
        r.refusal = "no room: " + outcome.refusal;
        return r;
    }
    r.ok = true;
    r.count = count;
    r.container = outcome.events.empty() ? "" : items::container_name(outcome.events.front().container);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::info("[gm] {}: {} x {} given by {} as {} (quality {}), {} storage event(s), first into {}", name(), count, r.item_name, by, r.rarity, r.quality, outcome.events.size(), r.container);
    return r;
}

/// The client calls of storage, using items (session/rpc_table.h).
void PlayerSession::register_items_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerRequestStorageEvent", [](PlayerSession& s, const RpcContext& c) { s.on_storage_event_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerRequestStorageData", [](PlayerSession& s, const RpcContext& c) { s.on_storage_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerItemUsed", [](PlayerSession& s, const RpcContext& c) { s.on_item_used(c.conn, c.rpc, c.now); });
    t.add(RpcScope::Pawn, "ServerActivateItem", [](PlayerSession& s, const RpcContext&) { s.on_activate_item_ignored(); });
}

}}  // namespace aoc::worldd
