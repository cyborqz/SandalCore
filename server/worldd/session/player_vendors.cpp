// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_vendors.cpp
//
//  Vendors and shops: opening a shop, the node tax table, buying, selling and repairing, and the game master purse.
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

// -- vendors, step 2: the shopkeepers who talk ----------------------------------------------------------------------------------------------------------

namespace {

/// What a shop's predicates are asked: the node the player stands in, the node the player is a citizen of and the node the shopkeeper stands in.
vendor::ShopFacts shop_facts_for(const world::CityNodes* nodes, const GoodPosition& player, const world::WorldActor* npc, int64_t citizen_node, int reputation_rank) {
    vendor::ShopFacts facts;
    facts.nodes = nodes;
    facts.player_x = player.x;
    facts.player_y = player.y;
    facts.citizen_node = citizen_node;
    facts.reputation_rank = reputation_rank;
    if (npc) {
        facts.has_npc = true;
        facts.npc_x = npc->x;
        facts.npc_y = npc->y;
    }
    return facts;
}

}  // namespace

bool PlayerSession::open_talk_shop(Connection& conn, const world::WorldActor& npc, int64_t def, Clock::time_point /*now*/) {
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const dialogue::Book* book = world_ ? world_->dialogue() : nullptr;
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        spdlog::log(level_for(vendor_logged_), "[session] {}: {}'s shop {} does not open: {}", name(), npc.npc.name, def, why);
        return false;
    };
    if (!shops || !shops->def(def) || !shops->is_open(def)) return refuse("worldd opens no such shop yet");
    // only a shop of the shopkeeper's own conversation
    if (!book || book->vendor_defs(npc.npc.dialogue_root).count(def) == 0) return refuse("the conversation of " + npc.npc.name + " opens no such shop");
    const GoodPosition at = checker_.good();
    const double dx = npc.x - at.x, dy = npc.y - at.y, dz = npc.z - at.z;
    if (dx * dx + dy * dy + dz * dz >= vendor::kShopRangeCm * vendor::kShopRangeCm) {
        return refuse("the shopkeeper is " + std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0)) + " m away (a shop opens within 12 m)");
    }
    // the window answers once: the line the server ran and the request the client may send for the same shop are one opening
    if (vendor_actor_ == npc.id && vendor_def_ == def && vendor_by_talk_) {
        spdlog::log(level_for(vendor_logged_), "[session] {}: the shop {} of {} is open already: not answered again", name(), def, npc.npc.name);
        return true;
    }
    if (!room_for(2)) return false;
    vendor_actor_ = npc.id;
    vendor_def_ = def;
    vendor_by_talk_ = true;
    ++vendor_opens_;
    // what the window hides or greys out for this player and this shopkeeper
    const vendor::ShopFacts facts =
        shop_facts_for(world_->city_nodes(), at, &npc, pb_.setup().citizen_node, reputation_rank_at(citizenship_node_at(npc.x, npc.y).first));
    bool unreadable = false;
    const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow& p) {
        const auto v = dialogue::evaluate_expression(p.expression, facts);
        if (!v) unreadable = true;
        return v && v->i != 0;
    };
    repl::UnavailableList unavailable = shops->unavailable(def, holds);
    size_t hidden_tabs = 0, listings = 0;
    for (const repl::UnavailableTab& t : unavailable) {
        hidden_tabs += t.inventory.hidden ? 1 : 0;
        listings += t.listings.size();
    }
    spdlog::log(level_for(vendor_logged_), "[session] {}: opens the shop {} of {} from the conversation: {} tab(s) listed as unavailable ({} hidden), {} listing(s){}", name(), shops->def(def)->name,
                npc.npc.name, unavailable.size(), hidden_tabs, listings, unreadable ? "; a predicate does not evaluate and does not hold" : "");
    const FIntrepidNetworkGUID guid = npc.actor.guid;
    queue("ClientOpenVendor", [def, guid, unavailable = std::move(unavailable)](bootstrap::PlayerBootstrap& pb) { return pb.open_vendor(def, guid, unavailable); }, conn, /*must_build=*/true);
    return true;
}

void PlayerSession::on_open_vendor(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto request = repl::parse_server_open_vendor(rpc.call);
    if (!request) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerOpenVendor does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerOpenVendor", now) || !room_for(2)) return;
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        spdlog::log(level_for(vendor_logged_), "[session] {}: the client's ServerOpenVendor for shop {} is refused: {}", name(), request->vendor, why);
    };
    if (dead_) return refuse("the character is dead");
    // the NPC the client may name is the one it talks to; the shopkeeper's own guid in the request is not what decides
    if (!talk_.open()) {
        // node citizenship: the join dialog's purchase button names the shop of the tax receipts of the Node Administrator who offered it, after the conversation went on
        if (const world::WorldActor* offered = citizenship_offer_npc(request->vendor, now)) {
            open_talk_shop(conn, *offered, request->vendor, now);
            return;
        }
        return refuse("no conversation is open");
    }
    const world::WorldActor* npc = registry_ ? registry_->find(talk_.state().source) : nullptr;
    if (!npc) return refuse("the NPC the player talks to is gone");
    open_talk_shop(conn, *npc, request->vendor, now);
}

// The vendor window waits for this answer (vendor/node_tax.h): the exe's UVendorMenu::InitializeVendor sent the request, and only the table's
// arrival makes the window fill its listings, taxes and funds.  Retail answers the next packet with the node's table.
void PlayerSession::on_node_tax_request(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto node = repl::parse_node_tax_request(rpc.call);
    ++tax_requests_;
    if (!node) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerGetNodeTaxTable does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(tax_budget_, "ServerGetNodeTaxTable", now) || !room_for(1)) return;
    if (dead_) return;   // the window closed with the death
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) {
        spdlog::log(level_for(tax_logged_), "[session] {}: the client asks for the taxes of node {} but has no vendor window open ({}): ignored", name(), *node, why);
        return;
    }
    const int64_t asked = *node;
    if (!a->npc.is_gm_vendor) {
        // a town shop: the table of a node nobody has changed, for a node the world knows; nothing for any other (the client's window then stays empty, and a made-up node id gets no answer)
        const world::CityNodes* nodes = world_ ? world_->city_nodes() : nullptr;
        if (nodes && !nodes->by_guid(asked)) {
            spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}, which the world does not know: no answer", name(), a->npc.name, asked);
            return;
        }
        ++tax_tables_sent_;
        const world::Elections* el = world_ ? world_->elections() : nullptr;
        auto table = std::make_shared<protocol::repl::NodeTaxDetails>(vendor::node_table(asked, el ? el->taxes(asked) : nullptr));
        spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}: sending its table ({} tags{})", name(), a->npc.name, asked,
                    table->entries.size(), el && el->taxes(asked) ? ", with the mayor's rates" : "");
        queue("ClientSetNodeTaxTable", [table](bootstrap::PlayerBootstrap& pb) { return pb.node_tax_table(*table); }, conn, /*must_build=*/true);
        return;
    }
    ++tax_tables_sent_;
    spdlog::log(level_for(tax_logged_), "[session] {}: the window of {} asks for the taxes of node {}: sending the untaxed table ({} tags)", name(), a->npc.name, *node,
                vendor::untaxed_table(*node).entries.size());
    queue("ClientSetNodeTaxTable", [asked](bootstrap::PlayerBootstrap& pb) { return pb.node_tax_table(vendor::untaxed_table(asked)); }, conn, /*must_build=*/true);
}

void PlayerSession::on_vendor_purchase(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto cart = repl::parse_vendor_purchase(rpc.call);
    if (!cart) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerHandleVendorPurchase does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerHandleVendorPurchase", now) || !room_for(6)) return;
    const char* what = "ServerHandleVendorPurchase";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    const vendor::GmVendors* gm = world_ ? world_->gm_vendors() : nullptr;
    if (dead_) return refuse("the character is dead: the vendor window closed with the death");
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) return refuse(why);
    // vendors, step 0: a town shop is paid for from the purse
    if (!a->npc.is_gm_vendor) return buy_from_town_shop(conn, *cart, now);
    if (!db || !gm) return refuse("worldd has no GM vendors");
    const vendor::GmVendor* def = gm->find(vendor_def_);
    if (!def) return refuse("the open window's vendor def is unknown");
    const size_t free = static_cast<size_t>(items::kBackpackSlots) - inventory_.count(svc::ItemContainer::Backpack);
    const vendor::PurchasePlan plan = gm->plan_purchase(*db, *def, *cart, free);
    if (!plan.ok) return refuse(plan.refusal);
    std::vector<svc::InventoryItem> fresh;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (const use::Granted& g : plan.stacks) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = g.record;
        it.instance_guid = fresh_guid();
        it.quantity = g.quantity;
        it.quality = g.quality;
        it.durability = g.durability;
        it.created_at_ms = created;
        // an instance guid must be new within this batch too (fresh_guid only looks at the inventory)
        for (const svc::InventoryItem& other : fresh) {
            if (other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
        fresh.push_back(std::move(it));
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok) return refuse(outcome.refusal);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_purchases_;
    const std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
    }
    spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} (free): {} storage event(s)", name(), plan.summary, outcome.events.size());
}
// ~ Cyb - C-O-A ~

// ============================================================================================================================
//  the vendors milestone, step 0: the purse and the general merchant
// ============================================================================================================================

int64_t PlayerSession::town_shop_of(const world::WorldActor& npc) const {
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    if (!shops || npc.kind != world::ActorKind::Npc || npc.npc.is_gm_vendor) return 0;
    return shops->shop_of(npc.npc.instance_guid, npc.npc.asset_guid);
}

void PlayerSession::buy_from_town_shop(Connection& conn, const repl::VendorPurchaseCart& cart, Clock::time_point now) {
    const char* what = "ServerHandleVendorPurchase";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!shops || !db) return refuse("worldd has no town shops");
    // the shop's predicates: the node the player stands in, and the node the shopkeeper stands in
    const GoodPosition at = checker_.good();
    const world::WorldActor* keeper = registry_ ? registry_->find(vendor_actor_) : nullptr;
    const int64_t keeper_node = keeper ? citizenship_node_at(keeper->x, keeper->y).first : 0;
    const vendor::ShopFacts facts = shop_facts_for(world_->city_nodes(), at, keeper, pb_.setup().citizen_node, reputation_rank_at(keeper_node));
    bool unreadable = false;
    const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow& p) {
        const auto v = dialogue::evaluate_expression(p.expression, facts);
        if (!v) unreadable = true;
        return v && v->i != 0;
    };
    // the shopkeeper's node: its mayor's rates, and its treasury takes the tax
    const int64_t shop_node = keeper_node;
    world::Elections* governed = world_->elections();
    const vendor::TownShops::Quote quote =
        shops->quote(vendor_def_, cart, reputation_taxed(shops->rate(vendor_def_, governed ? governed->taxes(shop_node) : nullptr), shop_node), holds);
    if (!quote.ok) {
        if (unreadable) spdlog::warn("[session] {}: a shop predicate does not evaluate: the listing is not sold", name());
        return refuse(quote.refusal);
    }
    // a price in Glint is paid with Glint T1 items of the bags (the material bags hold them); a price in money, from the purse
    const bool in_glint = quote.currency == items::kGlintTier;
    if (!in_glint && !wallet_.can_pay(quote.total)) {
        return refuse("the purse holds " + std::to_string(wallet_.copper()) + " copper and the cart costs " + std::to_string(quote.total) + " (not enough money)");
    }
    if (in_glint && inventory_.count_of(items::kGlintT1Item) < quote.total) {
        return refuse("the bags hold " + std::to_string(inventory_.count_of(items::kGlintT1Item)) + " Glint T1 and the cart costs " + std::to_string(quote.total) + " (not enough Glint)");
    }
    // the goods, as stacks, routed by the bag rule; a cart that does not fit whole is refused whole
    std::vector<svc::InventoryItem> fresh;
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() * 1000;
    for (const vendor::TownShops::Line& line : quote.lines) {
        std::string error;
        const auto rec = db->item(line.item, error);
        if (!rec) return refuse(error.empty() ? "content.db has no item record " + std::to_string(line.item) : "content.db failed: " + error);
        stacks_of(line.item, line.quantity, *rec, fresh, created);
    }
    for (svc::InventoryItem& it : fresh) {
        it.instance_guid = fresh_guid();
        // an instance guid must be new within this batch too (fresh_guid only looks at the inventory)
        for (const svc::InventoryItem& other : fresh) {
            if (&other != &it && other.instance_guid == it.instance_guid) it.instance_guid = fresh_guid();
        }
    }
    // paying in Glint: the stacks give first, and the goods must fit behind that; any failure puts the bags back as they were
    const items::Inventory kept = inventory_;
    items::Outcome paid;
    if (in_glint) {
        paid = inventory_.take(items::kGlintT1Item, quote.total);
        if (!paid.ok) return refuse(paid.refusal);
    }
    const items::Outcome outcome = inventory_.grant(std::move(fresh), item_catalog());
    if (!outcome.ok || (in_glint && paid.events.size() + outcome.events.size() > bootstrap::kMaxEventsPerChain)) {
        inventory_ = kept;
        return refuse(outcome.ok ? "the payment and the goods are more events than one answer holds" : outcome.refusal);
    }
    const int64_t before = wallet_.copper();
    if (!in_glint) wallet_.pay(quote.total);   // checked above: it cannot be short
    if (!in_glint && governed && shop_node != 0) {
        // the tax of a purchase in money goes to the node's treasury: what the cart costs less what it costs untaxed
        const vendor::TownShops::Quote untaxed = shops->quote(vendor_def_, cart, vendor::TaxRate{}, holds);
        if (untaxed.ok && quote.total > untaxed.total) {
            governed->add_treasury(shop_node, quote.total - untaxed.total);
            world_->save_elections();
        }
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_purchases_;
    if (in_glint) {
        // the retail answer to a purchase paid in items: the Remove of the Glint stack(s) (no toast), the callout in the Glint tier (no Currencies change), then the Add of the goods
        std::vector<bootstrap::ItemEvent> paid_events = item_events_for(paid);
        std::vector<bootstrap::ItemEvent> bought = item_events_for(outcome);
        add_item_toasts(bought, /*removals=*/false);
        const int64_t glint_delta = -quote.total;
        queue("purchase answer", [paid_events, bought, glint_delta](bootstrap::PlayerBootstrap& pb) { return pb.item_payment_answer(items::kGlintTier, glint_delta, paid_events, bought); }, conn, /*must_build=*/true);
        spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} line(s) at the shop for {} Glint T1: {} + {} storage event(s)", name(), quote.lines.size(), quote.total, paid.events.size(),
                    outcome.events.size());
        return;
    }
    // the answer: the callout first, then the Add events (in one chain), then the new balance in the pawn's Currencies; each Add carries the "Acquired" toast of the item feed
    std::vector<bootstrap::ItemEvent> events = item_events_for(outcome);
    add_item_toasts(events, /*removals=*/false);
    const int64_t delta = -quote.total;
    for (size_t at_event = 0; at_event < events.size(); at_event += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at_event),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at_event + bootstrap::kMaxEventsPerChain)));
        if (at_event == 0) {
            queue("purchase answer", [part, delta](bootstrap::PlayerBootstrap& pb) { return pb.purchase_answer(items::kMoneyTier, delta, part); }, conn, /*must_build=*/true);
        } else {
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: bought {} line(s) at the shop for {} copper ({} -> {}): {} storage event(s)", name(), quote.lines.size(), quote.total, before,
                wallet_.copper(), outcome.events.size());
}

// ============================================================================================================================
//  the vendors milestone, step 1: selling, repairing and the item toasts
// ============================================================================================================================

void PlayerSession::on_vendor_transaction(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const auto cart = repl::parse_vendor_transaction(rpc.call);
    if (!cart) {
        if (!vendor_unreadable_logged_) {
            vendor_unreadable_logged_ = true;
            spdlog::warn("[session] {}: a ServerHandleVendorTransaction does not read ({} parameter bits): ignored (further ones are not logged)", name(), rpc.call.param_bits);
        }
        return;
    }
    if (!within_budget(vendor_budget_, "ServerHandleVendorTransaction", now) || !room_for(6)) return;
    const char* what = "ServerHandleVendorTransaction";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    if (dead_) return refuse("the character is dead: the vendor window closed with the death");
    std::string why;
    const world::WorldActor* a = open_vendor(why);
    if (!a) return refuse(why);
    if (a->npc.is_gm_vendor) {
        if (!vendor_sale_logged_) {
            vendor_sale_logged_ = true;
            spdlog::info("[session] {}: a sale to a vendor: GM vendors buy nothing, ignored (further ones are not logged)", name());
        }
        return;
    }
    if (cart->vendor != vendor_def_) return refuse("the cart names another shop than the one that is open");
    if (cart->operation == 2) return sell_to_town_shop(conn, *cart, now);
    if (cart->operation == 4) return repair_at_town_shop(conn, *cart, now);
    return refuse("an operation of " + std::to_string(cart->operation) + " is not made (a sale is 2, a repair 4)");
}

void PlayerSession::sell_to_town_shop(Connection& conn, const repl::VendorTransactionCart& cart, Clock::time_point now) {
    const char* what = "ServerHandleVendorTransaction";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!shops || !db) return refuse("worldd has no town shops");
    if (cart.instances.empty() || cart.instances.size() > vendor::kMaxShopCartLines) return refuse("a sale of " + std::to_string(cart.instances.size()) + " items");
    // every item first: it is the character's, in a bag, one the shop buys; the whole stack is sold
    std::vector<svc::InventoryItem> sold;
    std::set<std::string> seen;
    int64_t total = 0;
    for (const repl::GuidValue& g : cart.instances) {
        const std::string guid = items::guid_hex(g);
        if (!seen.insert(guid).second) return refuse("the same item twice in one sale");
        const svc::InventoryItem* held = inventory_.find_guid(guid);
        if (!held) return refuse("an item that is in none of this character's bags");
        if (!is_bag(held->container)) return refuse("an item that is worn or in the quest storage is not sold");
        std::string error;
        const auto facts = db->item_sale(held->item_record_id, error);
        if (!facts) return refuse(error);
        const vendor::SaleQuote quote = vendor::quote_sale(shops->data(), vendor_def_, held->item_record_id, held->quantity, held->quality, *facts, items::kMoneyTier, progress_.level);
        if (!quote.ok) return refuse("item " + std::to_string(held->item_record_id) + " is not sold: " + quote.refusal);
        total += quote.total;
        if (total > items::kMoneyCap) break;
        sold.push_back(*held);
    }
    if (total > wallet_.room()) {
        return refuse("the sale comes to " + std::to_string(total) + " copper and the purse holds " + std::to_string(wallet_.copper()) + " of at most " + std::to_string(items::kMoneyCap) +
                      " (a sale past the cap is refused whole)");
    }
    // the stacks leave; all or nothing
    const items::Inventory backup = inventory_;
    items::Outcome all;
    all.ok = true;
    for (const svc::InventoryItem& it : sold) {
        items::Outcome o = inventory_.consume(it.container, it.slot, it.instance_guid, it.quantity);
        if (!o.ok) {
            inventory_ = backup;
            return refuse(o.refusal);
        }
        all.events.insert(all.events.end(), std::make_move_iterator(o.events.begin()), std::make_move_iterator(o.events.end()));
    }
    const int64_t before = wallet_.copper();
    wallet_.give(total);
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_sales_;
    // the answer, in the order of a sale: the Remove events (each with its "Removed" toast), then the money callout; then the new balance in the pawn's Currencies
    std::vector<bootstrap::ItemEvent> events = item_events_for(all);
    add_item_toasts(events, /*removals=*/true);
    for (size_t at = 0; at < events.size(); at += bootstrap::kMaxEventsPerChain) {
        const std::vector<bootstrap::ItemEvent> part(events.begin() + static_cast<std::ptrdiff_t>(at),
                                                     events.begin() + static_cast<std::ptrdiff_t>(std::min(events.size(), at + bootstrap::kMaxEventsPerChain)));
        if (at + bootstrap::kMaxEventsPerChain >= events.size()) {
            queue("sale answer", [part, total](bootstrap::PlayerBootstrap& pb) { return pb.sale_answer(items::kMoneyTier, total, part); }, conn, /*must_build=*/true);
        } else {
            queue("ClientReceiveStorageEvent", [part](bootstrap::PlayerBootstrap& pb) { return pb.item_events(part); }, conn, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: sold {} stack(s) to the shop for {} copper ({} -> {}): {} storage event(s)", name(), sold.size(), total, before, wallet_.copper(),
                all.events.size());
}

void PlayerSession::repair_at_town_shop(Connection& conn, const repl::VendorTransactionCart& cart, Clock::time_point now) {
    const char* what = "ServerHandleVendorTransaction";
    const auto refuse = [&](const std::string& why) {
        ++vendor_refused_;
        refuse_item_request(conn, what, why, now);
    };
    const vendor::TownShops* shops = world_ ? world_->town_shops() : nullptr;
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!shops || !db) return refuse("worldd has no town shops");
    if (cart.instances.empty() || cart.instances.size() > vendor::kMaxShopCartLines) return refuse("a repair of " + std::to_string(cart.instances.size()) + " items");
    const world::WorldActor* keeper = registry_ ? registry_->find(vendor_actor_) : nullptr;
    const int64_t shop_node = keeper ? citizenship_node_at(keeper->x, keeper->y).first : 0;   // its mayor's rates; its treasury takes the tax
    world::Elections* governed = world_->elections();
    const vendor::TaxRate tax = reputation_taxed(shops->rate(vendor_def_, governed ? governed->taxes(shop_node) : nullptr), shop_node);
    int64_t untaxed_total = 0;
    struct Mended {
        svc::InventoryItem item;
        float              full = 0;
    };
    std::vector<Mended> mended;
    std::set<std::string> seen;
    int64_t total = 0;
    for (const repl::GuidValue& g : cart.instances) {
        const std::string guid = items::guid_hex(g);
        if (!seen.insert(guid).second) return refuse("the same item twice in one repair");
        const svc::InventoryItem* held = inventory_.find_guid(guid);
        if (!held) return refuse("an item that is in none of this character's storages");
        if (held->container != svc::ItemContainer::Backpack && held->container != svc::ItemContainer::Equipment) return refuse("only an item in the item bag or worn is repaired");
        std::string error;
        const auto rec = db->item(held->item_record_id, error);
        if (!rec) return refuse(error.empty() ? "content.db has no item record " + std::to_string(held->item_record_id) : error);
        const auto facts = db->item_sale(held->item_record_id, error);
        if (!facts) return refuse(error);
        const float full = rec->max_durability_at(held->quality);
        const vendor::RepairQuote quote = vendor::quote_repair(shops->data(), vendor_def_, held->item_record_id, held->quality, static_cast<double>(held->durability), static_cast<double>(full),
                                                               *facts, items::kMoneyTier, tax, progress_.level);
        if (!quote.ok) return refuse("item " + std::to_string(held->item_record_id) + " is not repaired: " + quote.refusal);
        total += quote.total;
        const vendor::RepairQuote untaxed = vendor::quote_repair(shops->data(), vendor_def_, held->item_record_id, held->quality, static_cast<double>(held->durability),
                                                                 static_cast<double>(full), *facts, items::kMoneyTier, vendor::TaxRate{}, progress_.level);
        untaxed_total += untaxed.ok ? untaxed.total : quote.total;
        mended.push_back({*held, full});
    }
    if (!wallet_.can_pay(total)) {
        return refuse("the purse holds " + std::to_string(wallet_.copper()) + " copper and the repair costs " + std::to_string(total) + " (not enough money)");
    }
    // the wear is mended to full; the client is told with one durability update per storage and the money callout
    struct Update {
        bootstrap::ItemStorage storage;
        int32_t                slot;
        float                  durability;
    };
    std::vector<Update> updates;
    for (const Mended& m : mended) {
        if (!inventory_.set_durability(m.item.container, m.item.slot, m.item.instance_guid, m.full)) return refuse("the item's durability could not be set");
        updates.push_back({bootstrap::item_storage_of(m.item.container), m.item.slot, m.full});
    }
    const int64_t before = wallet_.copper();
    wallet_.pay(total);   // checked above: it cannot be short
    if (governed && shop_node != 0 && total > untaxed_total) {
        governed->add_treasury(shop_node, total - untaxed_total);   // the repair's tax to the node's treasury
        world_->save_elections();
    }
    inventory_dirty_ = true;
    inventory_changed_ = true;
    ++items_applied_;
    ++vendor_repairs_;
    for (size_t i = 0; i < updates.size(); ++i) {
        const Update u = updates[i];
        if (i + 1 < updates.size()) {
            queue("ClientUpdateItemDurability", [u](bootstrap::PlayerBootstrap& pb) { return pb.item_durability(u.storage, u.slot, u.durability); }, conn, /*must_build=*/true);
        } else {
            const int64_t delta = -total;
            queue("repair answer", [u, delta](bootstrap::PlayerBootstrap& pb) { return pb.repair_answer(u.storage, u.slot, u.durability, items::kMoneyTier, delta); }, conn, /*must_build=*/true);
        }
    }
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::log(level_for(vendor_logged_), "[session] {}: repaired {} item(s) at the shop for {} copper ({} -> {})", name(), mended.size(), total, before, wallet_.copper());
}

PlayerSession::GmMoneyResult PlayerSession::gm_give_money(Connection& conn, int64_t copper, Clock::time_point now, const std::string& by) {
    (void)now;
    GmMoneyResult r;
    r.previous = wallet_.copper();
    r.balance = r.previous;
    if (copper < 1) {
        r.refusal = "the amount must be at least 1 copper";
        return r;
    }
    if (wallet_.room() == 0) {
        r.refusal = name() + "'s purse is full (" + std::to_string(items::kMoneyCap) + " copper, 1,000 gold)";
        return r;
    }
    if (!room_for(3)) {
        r.refusal = name() + "'s connection is busy: try again in a moment";
        return r;
    }
    r.added = wallet_.give(copper);
    r.balance = wallet_.copper();
    r.ok = true;
    inventory_dirty_ = true;
    inventory_changed_ = true;
    pb_.set_currency(items::kCopperCurrency, wallet_.copper());
    const int64_t shown = r.added;
    queue("ClientShowCurrencyCallout", [shown](bootstrap::PlayerBootstrap& pb) { return pb.currency_callout(items::kMoneyTier, shown); }, conn, /*must_build=*/true);
    queue("Currencies", [](bootstrap::PlayerBootstrap& pb) { return pb.currencies_update(); }, conn, /*must_build=*/true);
    spdlog::info("[gm] {}: {} copper added by {} ({} -> {}{})", name(), r.added, by, r.previous, r.balance, r.added < copper ? "; the purse is full, the rest is not added" : "");
    return r;
}

/// The client calls of vendors and the node tax table (session/rpc_table.h).
void PlayerSession::register_vendors_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerHandleVendorPurchase", [](PlayerSession& s, const RpcContext& c) { s.on_vendor_purchase(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerOpenVendor", [](PlayerSession& s, const RpcContext& c) { s.on_open_vendor(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerGetNodeTaxTable", [](PlayerSession& s, const RpcContext& c) { s.on_node_tax_request(c.conn, c.rpc, c.now); });
    t.add(RpcScope::PlayerController, "ServerHandleVendorTransaction", [](PlayerSession& s, const RpcContext& c) { s.on_vendor_transaction(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
