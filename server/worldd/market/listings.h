// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/listings.h
//
//  What a market holds between a listing and its sale: one Listing per registered item stack (the node, the seller, the
//  listing's own guid, the stack as it was listed, the item's name, the price in copper, the days it is listed and the
//  game's status) and the MarketStore the sessions read.  MemoryMarketStore keeps the listings in memory, for a test or a world
//  with no character service; the world's own store (market/market_store.h) saves them with the seller's bags and fills a
//  MemoryMarketStore as its cache.
// ============================================================================
#pragma once

#include "market/storefront_text.h"
#include "protocol/repl/value.h"
#include "svc/services.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc::worldd::market {

/// EMarketplaceItemStatus: LISTED (0) is the only status a listing is made with.
inline constexpr uint8_t kListed = 0;
/// Three days, the life of a listing the recordings show.
inline constexpr double kListingSeconds = 259200.0;
/// The most active listings one seller may hold in this world (the owner's choice; the game's data shows no cap).
inline constexpr size_t kMaxListingsPerSeller = 10;
/// The sales tax rate of a node that has no stored rate (the game's TaxDefaultValue), and the game's limits of a rate.  Each node stores its own, so that the mayors' feature only has
/// to change the stored one.
inline constexpr float kDefaultTaxRate = 0.5f;
inline constexpr float kMinTaxRate = 0.1f;
inline constexpr float kMaxTaxRate = 1.0f;
inline bool valid_tax_rate(float rate) { return rate >= kMinTaxRate && rate <= kMaxTaxRate; }   // (false for NaN)

/// One registered listing: the market's node, its seller (the character's guid and name), the listing's own guid, the stack
/// as it was listed (its quantity is the listed quantity), the name the client shows, the unit price in copper and the dates.
struct Listing {
    int64_t              node = 0;
    ::aoc::protocol::repl::GuidValue seller;
    std::string          seller_name;
    ::aoc::protocol::repl::GuidValue guid;
    svc::InventoryItem   item;
    std::string          item_name;
    int64_t              price_per_unit = 0;
    double               issue_date = 0;
    double               expiration_date = 0;
    uint8_t              status = kListed;
    /// The rarity the listed stack shows (EItemRarity: Common 0 ... Artifact 6): its record's quality curve at the stack's quality (items/rarity.h).  A search by rarity reads it.
    uint8_t              rarity = 0;
    bool operator==(const Listing&) const = default;
};

/// What a search sends for "any rarity" (the client's dropdown: its first choice), and the first value that is none (EItemRarity has 0 to 6).
inline constexpr int kAnyRarity = 7;

/// One storefront: the seller and the day his first listing at that market was made (the header's issue date), and the description the seller gave it at that market (empty:
/// none, the page shows the default "<name>'s Storefront").
struct Storefront {
    ::aoc::protocol::repl::GuidValue seller;
    std::string          seller_name;
    double               issue_date = 0;
    std::string          description;
    bool operator==(const Storefront&) const = default;
};

/// A seller's edit of the description of his storefront at one market: stored in the character service in one step, which also raises the market's page version.
struct DescribeRequest {
    int64_t                          node = 0;
    ::aoc::protocol::repl::GuidValue seller;
    std::string                      seller_name;
    std::string                      description;   // "" clears it
};

/// One listing's save: the listing as the session made it, its dates in Unix milliseconds, the fee it took, and the seller's bags, purse, jobs, recipes and warehouse as they
/// are after the listing took its units and its fee.
struct ListingSave {
    Listing                          listing;
    int64_t                          issue_date_ms = 0;
    int64_t                          expiry_date_ms = 0;
    int64_t                          fee = 0;
    std::vector<svc::InventoryItem>  items;
    std::vector<svc::WalletEntry>    wallet;
    std::vector<svc::ProcessingJob>  jobs;
    std::vector<int64_t>             recipes;
    std::vector<svc::WarehouseItem>  warehouse;
};

/// The answer that settles a step (a listing's save, a purchase, a removal, an expiry): the call's status and, for Ok, how the step stands (svc::MarketOutcome: stored, or refused
/// for a reason of the listing with nothing written).  A listing's save has no outcome of its own: it is Done when the call is Ok.
struct ListingSaveAnswer {
    svc::CallStatus    status = svc::CallStatus::Failed;
    std::string        reason;
    svc::MarketOutcome outcome = svc::MarketOutcome::Unspecified;
    int32_t            available = 0;      // a refusal for the units: what the listing holds
    std::string        seller_id;          // a purchase or an expiry that was stored: the seller, whose mail changed
    /// A description edit: Done (written) and Replayed (the storefront held this text already) are stored, ListingGone says the seller has no listing at the market; `description` is
    /// the text the storefront holds now and `header_raises` the market's page-version count now.
    std::string        description;
    int64_t            header_raises = 0;
    /// Whether the step is in the database now.
    bool stored() const { return status == svc::CallStatus::Ok && svc::market_outcome_stored(outcome); }
};

/// Whether an answer of the character service settles a save: the store has it (Ok), or it does not and will not (a refusal, an unknown character, a service that keeps no
/// listings).  Unavailable and Failed settle nothing: the store may or may not have written it.
inline bool is_clear(svc::CallStatus status) {
    return status == svc::CallStatus::Ok || status == svc::CallStatus::Refused || status == svc::CallStatus::NotFound || status == svc::CallStatus::Unsupported;
}

/// The steps a character's session has in flight at the market: a listing's save, a purchase and a removal (an expiry is the world's own, not a session's).
enum class StepKind : uint8_t { Register, Buy, Remove };

/// A seller's bags, purse and the rest as a session leaves them.
struct BagSnapshot {
    std::vector<svc::InventoryItem>  items;
    std::vector<svc::WalletEntry>    wallet;
    std::vector<svc::ProcessingJob>  jobs;
    std::vector<int64_t>             recipes;
    std::vector<svc::WarehouseItem>  warehouse;
};

/// One step whose save is not decided yet, as a session keeps it: the save's ticket, the listing and, for a listing's save, the stack before the units left it, how many left and
/// the fee.  A purchase or a removal carries, for a session that ends before the answer, the bags the character ends with if the step is refused (`without`).
struct PendingListing {
    uint64_t           ticket = 0;
    Listing            listing;
    svc::InventoryItem taken;       // the stack before the units left it
    int32_t            quantity = 0;   // the units the step moves
    int64_t            fee = 0;
    StepKind           kind = StepKind::Register;
    BagSnapshot        without;        // Buy, Remove: the bags had the step been refused
};

/// What a session that ends while listing saves are in flight hands to the world: the saves, oldest first, and the bags it ends with.  The world takes each save's answer,
/// makes the listing or gives back what it took, and then writes the bags.
struct HandOver {
    std::string                      character_id;
    ::aoc::protocol::repl::GuidValue seller;
    std::vector<PendingListing>      listings;
    BagSnapshot                      bags;
};

/// The listings of a market, as the sessions read and add them.  MemoryMarketStore is this server's; a store over the
/// character service replaces it behind the same calls (the market plan's MarketApi).
class MarketStore {
public:
    virtual ~MarketStore() = default;
    virtual void add(const Listing& listing) = 0;
    /// One storefront per seller with a listing at `node`, in the order each seller first listed there.
    virtual std::vector<Storefront> storefronts(int64_t node) const = 0;
    /// The listings of `seller` at `node`, in the order they were made.
    virtual std::vector<Listing> listings(int64_t node, const ::aoc::protocol::repl::GuidValue& seller) const = 0;
    /// How many active listings `seller` holds in this world, at every market.
    virtual size_t count(const ::aoc::protocol::repl::GuidValue& seller) const = 0;
    /// The listing with `guid` (nullptr: none).
    virtual const Listing* find(const ::aoc::protocol::repl::GuidValue& guid) const = 0;
    virtual size_t size() const = 0;
    /// The storefronts at `node` that list an item whose name holds `text` (whatever the case) and whose rarity is `rarity` (kAnyRarity: any), without `except`'s own, in the order
    /// of storefronts().  The text is not empty.  Reads the cache: no database is asked.
    virtual std::vector<Storefront> search(int64_t node, std::string_view text, int rarity, const ::aoc::protocol::repl::GuidValue& except) const {
        (void)node; (void)text; (void)rarity; (void)except;
        return {};
    }
    /// How many times the storefront pages of `node` changed (a description edit raises it): the page version a client holds is the game's own 1843 plus this.
    virtual uint64_t header_raises(int64_t) const { return 0; }
    /// Sets the description the cache holds for a storefront ("" takes it away): the world's own copy of what the character service stored, which the storefront pages read.  A
    /// test sets one to see the pages carry it.
    virtual void set_description(int64_t, const ::aoc::protocol::repl::GuidValue&, const std::string&) {}
    /// The rate of the sales tax of `node`: the one its row stores, or the game's default.
    virtual float node_rate(int64_t) const { return kDefaultTaxRate; }
    /// Whether a purchase, a removal or an expiry of the listing is in flight (one step at a time for a listing).
    virtual bool listing_busy(const ::aoc::protocol::repl::GuidValue&) const { return false; }
    /// Whether the saved listings were read: until they are, a registration is refused and no storefront answers.  A store that keeps them in memory is ready.
    virtual bool loaded() const { return true; }
    /// Queues one listing's save; its ticket names the answer.  0: the store cannot save it.
    virtual uint64_t submit_listing(ListingSave) { return 0; }
    /// Queues a purchase or a removal (the request carries the character's bags after the step); the ticket names the answer, taken with take_listing.  0: the store cannot, or a
    /// step of the listing is in flight, or a log-in of the character waits.  `listing` is the listing as the world holds it now.
    virtual uint64_t submit_buy(svc::BuyRequest, const Listing&) { return 0; }
    virtual uint64_t submit_remove(svc::RemoveRequest, const Listing&) { return 0; }
    /// Asks for the expiry of a listing that is past its time at once, unless a step of it is in flight; the world's market worker makes the call.
    virtual void sweep_now(const ::aoc::protocol::repl::GuidValue&) {}
    /// Takes a ticket's answer when the save has it (a clear one: the store asks for an unclear one again); false while it has not.  The listing joins the cache when the
    /// answer is Ok.
    virtual bool take_listing(uint64_t, ListingSaveAnswer&) { return false; }
    /// Drops a ticket and its answer.
    virtual void forget_listing(uint64_t) {}
    /// Queues a seller's edit of his storefront's description; its ticket names the answer, taken with take_describe.  0: the store cannot (no service, or the listings are not read).
    /// The edit is one call of the character service, which also raises the market's page version; the answer joins the cache when it is taken.
    virtual uint64_t submit_describe(DescribeRequest) { return 0; }
    /// Takes an edit's answer when the service has it (a clear one: the store asks again for an unclear one); false while it has not.  A stored edit joins the cache.
    virtual bool take_describe(uint64_t, ListingSaveAnswer&) { return false; }
    /// A session that ends with edits in flight hands their tickets over: the world takes their answers (the cache must hear of an edit the service stored).
    virtual void hand_over_describes(std::vector<uint64_t>) {}
    /// A session that ends with saves in flight hands them over with the bags it ends with (see HandOver); the world finishes them.
    virtual void hand_over(HandOver) {}
    /// Whether a save handed over by an ended session of `seller` is still being finished: the seller makes no new listing meanwhile.
    virtual bool busy(const ::aoc::protocol::repl::GuidValue&) const { return false; }
    /// Whether a listing save of the character `character_id` (32 lowercase hex digits) has no answer taken yet, or a session that ended with one has not queued its bags yet.
    /// Called from the log-in thread, so it is safe to call from any thread: a log-in waits while this is true (with the mail claims and the sink's queued writes, in one wait:
    /// PlayerWorld::character_writes_pending), so it never loads bags a save can still change.
    virtual bool saving(const std::string&) const { return false; }
};

/// The containers an item may be listed from: the backpack and the resource containers, the places the Sell tab lists from.  Worn gear and the quest storage are refused with
/// their own answers, and so is any container this list does not name.
inline bool sellable_container(svc::ItemContainer c) {
    return c == svc::ItemContainer::Backpack || c == svc::ItemContainer::Resource0 || c == svc::ItemContainer::Resource1 || c == svc::ItemContainer::Resource2;
}

/// The world's listings, in memory: they are lost when the server restarts.  Four indexes (by node, by seller at a node,
/// by seller, and by guid) keep the window's answers from walking every listing.
class MemoryMarketStore final : public MarketStore {
public:
    void add(const Listing& listing) override {
        const size_t index = listings_.size();
        listings_.push_back(listing);
        by_node_[listing.node].push_back(index);
        by_seller_at_node_[SellerAtNode{key_of(listing.seller), listing.node}].push_back(index);
        by_seller_[key_of(listing.seller)].push_back(index);
        by_guid_[key_of(listing.guid)] = index;
    }

    std::vector<Storefront> storefronts(int64_t node) const override {
        std::vector<Storefront> out;
        const auto it = by_node_.find(node);
        if (it == by_node_.end()) return out;
        for (size_t index : it->second) {
            const Listing& l = listings_[index];
            auto at = out.begin();
            for (; at != out.end(); ++at) {
                if (at->seller == l.seller) break;
            }
            if (at == out.end()) {
                out.push_back(Storefront{l.seller, l.seller_name, l.issue_date, description_at(node, l.seller)});
            } else if (l.issue_date < at->issue_date) {
                at->issue_date = l.issue_date;
            }
        }
        return out;
    }

    /// The storefronts at `node` with a listing whose name holds `text` at the rarity asked for (see MarketStore::search).
    std::vector<Storefront> search(int64_t node, std::string_view text, int rarity, const ::aoc::protocol::repl::GuidValue& except) const override {
        std::vector<Storefront> out;
        const auto it = by_node_.find(node);
        if (it == by_node_.end() || text.empty()) return out;
        const std::string needle = folded(text);
        std::set<SellerKey> matching;
        for (size_t index : it->second) {
            const Listing& l = listings_[index];
            if (l.seller == except) continue;
            if (rarity != kAnyRarity && l.rarity != rarity) continue;
            if (contains_folded(l.item_name, needle)) matching.insert(key_of(l.seller));
        }
        if (matching.empty()) return out;
        for (Storefront& sf : storefronts(node)) {
            if (matching.count(key_of(sf.seller)) != 0) out.push_back(std::move(sf));
        }
        return out;
    }

    /// The description `seller` gave his storefront at `node` ("" when none).
    std::string description_at(int64_t node, const ::aoc::protocol::repl::GuidValue& seller) const {
        const auto it = descriptions_.find(SellerAtNode{key_of(seller), node});
        return it == descriptions_.end() ? std::string() : it->second;
    }
    /// Sets the description of a storefront ("" takes it away).  The listings and their indexes are not touched.
    void set_description(int64_t node, const ::aoc::protocol::repl::GuidValue& seller, const std::string& text) override {
        if (text.empty()) descriptions_.erase(SellerAtNode{key_of(seller), node});
        else descriptions_[SellerAtNode{key_of(seller), node}] = text;
    }
    uint64_t header_raises(int64_t node) const override {
        const auto it = raises_.find(node);
        return it == raises_.end() ? 0 : it->second;
    }
    /// Sets a node's count of page changes when it is higher than the one held: a count never goes down.
    void raise_header_count(int64_t node, uint64_t raises) {
        uint64_t& held = raises_[node];
        if (raises > held) held = raises;
    }

    std::vector<Listing> listings(int64_t node, const ::aoc::protocol::repl::GuidValue& seller) const override {
        std::vector<Listing> out;
        const auto it = by_seller_at_node_.find(SellerAtNode{key_of(seller), node});
        if (it == by_seller_at_node_.end()) return out;
        out.reserve(it->second.size());
        for (size_t index : it->second) out.push_back(listings_[index]);
        return out;
    }

    size_t count(const ::aoc::protocol::repl::GuidValue& seller) const override {
        const auto it = by_seller_.find(key_of(seller));
        return it == by_seller_.end() ? 0 : it->second.size();
    }

    const Listing* find(const ::aoc::protocol::repl::GuidValue& guid) const override {
        const auto it = by_guid_.find(key_of(guid));
        return it == by_guid_.end() ? nullptr : &listings_[it->second];
    }

    size_t size() const override { return listings_.size(); }

    /// Drops every listing and index, and every description and count: a store over the character service fills the cache again from the database's rows.
    void clear() {
        clear_listings();
        descriptions_.clear();
        raises_.clear();
    }

    /// Takes the listing with `guid` out (a sold, removed or expired listing); false when there is none.  The indexes are made again, so a pointer find() gave is not valid after it.
    bool remove(const ::aoc::protocol::repl::GuidValue& guid) {
        const auto it = by_guid_.find(key_of(guid));
        if (it == by_guid_.end()) return false;
        std::vector<Listing> kept;
        kept.reserve(listings_.size());
        for (size_t i = 0; i < listings_.size(); ++i) {
            if (i != it->second) kept.push_back(std::move(listings_[i]));
        }
        clear_listings();   // (the descriptions stay: a seller whose listings sold out keeps his text for the next listing)
        for (const Listing& l : kept) add(l);
        return true;
    }

    /// The listings (up to `limit`, oldest first) whose time has come at `clock_now` (the synchronized clock): the expiry sweep's candidates.
    std::vector<Listing> expired(double clock_now, size_t limit) const {
        std::vector<Listing> out;
        for (const Listing& l : listings_) {
            if (l.status != kListed || !(l.expiration_date > 0) || l.expiration_date > clock_now) continue;
            out.push_back(l);
            if (out.size() >= limit) break;
        }
        return out;
    }

    /// Sets the units of the listing with `guid` (a part of it was sold); false when there is none or the quantity is not above 0.
    bool set_quantity(const ::aoc::protocol::repl::GuidValue& guid, int32_t quantity) {
        const auto it = by_guid_.find(key_of(guid));
        if (it == by_guid_.end() || quantity < 1) return false;
        listings_[it->second].item.quantity = quantity;
        return true;
    }

private:
    using SellerKey = std::array<uint32_t, 4>;
    using SellerAtNode = std::pair<SellerKey, int64_t>;

    static SellerKey key_of(const ::aoc::protocol::repl::GuidValue& g) { return {g.a, g.b, g.c, g.d}; }

    void clear_listings() {
        listings_.clear();
        by_node_.clear();
        by_seller_at_node_.clear();
        by_seller_.clear();
        by_guid_.clear();
    }

    std::map<SellerAtNode, std::string>             descriptions_;   // the description of a storefront, by (seller, node); a storefront with none has no entry
    std::map<int64_t, uint64_t>                     raises_;         // how many times a node's storefront pages changed
    std::vector<Listing>                            listings_;
    std::map<int64_t, std::vector<size_t>>          by_node_;
    std::map<SellerAtNode, std::vector<size_t>>     by_seller_at_node_;
    std::map<SellerKey, std::vector<size_t>>        by_seller_;
    std::map<SellerKey, size_t>                     by_guid_;
};

}  // namespace aoc::worldd::market
