// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_db.inc: the character service's Marketplace tables as a test double (MarketDb)
//  (included by area_session_market_sales_tests.inc)
//
//  MarketDb keeps what the service keeps: every character's stored bags and purse, the listings with their status, the mails with their money and items, the nodes' rates and totals
//  and the purchases' ids, and answers the world's calls the way the service does: a purchase, a removal and an expiry each change all the rows they name or none, a purchase
//  asked for again is answered "replayed" and writes nothing, and a listing that is not listed any more, is past its time, or has fewer units is refused with its outcome.  The real
//  service's rules are proved against PostgreSQL in services/internal/character/market_trade_test.go; this double lets the world's side be tested end to end: the relog, the restart and
//  the answers that are lost.
// ============================================================================
#include "items/wallet.h"
#include "svc/services.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace market_db {

inline int64_t wall_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

inline constexpr int32_t kListed = 0, kSold = 1, kRemoved = 2, kExpiredClaimed = 4;

struct Row {
    svc::MarketListing l;
    int32_t            status = kListed;
};

struct MailRow {
    std::string owner;
    svc::Mail   mail;
    std::string listing;      // a sale mail's listing
    int32_t     sold = 0;
};

}  // namespace market_db

struct MarketDb : RecordingSink {
    mutable std::mutex      mu;
    std::condition_variable cv;
    std::vector<market_db::Row>     rows;
    std::map<std::string, std::vector<svc::InventoryItem>> bags;     // the stored bags of each character
    std::map<std::string, std::vector<svc::WalletEntry>>   purses;   // ... and the purse
    std::vector<market_db::MailRow> mails;
    std::map<int64_t, float>   rates;        // the node's stored rate (none: 0.5)
    std::map<int64_t, int64_t> collected;    // the tax each node has taken in
    std::map<std::string, std::string> purchases;   // the purchase id and the listing it was made for
    std::map<std::string, std::string> names;       // a character's name, for a listing's seller
    uint64_t                   next_id = 1;

    // what a test sets to hold or spoil a step
    bool hold_buy = false, hold_remove = false, hold_expire = false, hold_claim = false;
    int  lose_buy_answers = 0;        // the next n purchases are stored and the answer is lost (Unavailable): the world asks again with the same id
    bool lose_all_answers = false;    // every purchase's answer is lost, a repeat of it too: the world never hears
    int  fail_buy_before = 0;         // the next n purchases are not stored and not answered (Unavailable)
    int  lose_remove_answers = 0;
    int  fail_expire_before = 0;
    bool started = false;

    // what it saw
    int buy_calls = 0, remove_calls = 0, expire_calls = 0, claim_calls = 0, listing_saves = 0;
    int claim_money_asks = 0, claim_changed_refusals = 0;   // the claims that named money, and the ones refused because the mail held another amount
    std::vector<svc::BuyRequest>    buys;
    std::vector<svc::RemoveRequest> removes;
    std::vector<std::string>        expired_ids;

    std::chrono::milliseconds retry_delay() const override { return std::chrono::milliseconds(1); }
    bool character_busy(const std::string&) const override { return false; }

    // ---- the test's own reads and seeds -----------------------------------------------------------------------------------------------
    std::string fresh() {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%032llx", static_cast<unsigned long long>(0xfa00000000ull + next_id++));
        return buf;
    }
    /// Seeds a listing of `seller` (a character this world has no session of, or one it has): `quantity` units of `record` at `price` copper each, listed `issued_ago_ms` ago, ending `expires_in_ms` from now.
    std::string seed_listing(const std::string& seller, const std::string& seller_name, int64_t node, int64_t record, int32_t quantity, int64_t price, int64_t expires_in_ms = 72ll * 3600 * 1000,
                             int64_t issued_ago_ms = 1000) {
        std::lock_guard<std::mutex> lock(mu);
        market_db::Row row;
        row.l.id = fresh();
        row.l.seller_id = seller;
        row.l.seller_name = seller_name;
        row.l.node = node;
        row.l.item.container = svc::ItemContainer::Backpack;
        row.l.item.item_record_id = record;
        row.l.item.instance_guid = fresh();
        row.l.item.quantity = quantity;
        row.l.item.quality = 1;
        row.l.item.durability = 11;
        row.l.item.created_at_ms = 1'771'299'474'000ll;
        row.l.price_per_unit = price;
        row.l.status = 0;
        row.l.issue_date_ms = market_db::wall_ms() - issued_ago_ms;
        row.l.expiry_date_ms = market_db::wall_ms() + expires_in_ms;
        rows.push_back(row);
        names[seller] = seller_name;
        return row.l.id;
    }
    market_db::Row* find_row(const std::string& id) {
        for (market_db::Row& r : rows) {
            if (r.l.id == id) return &r;
        }
        return nullptr;
    }
    int32_t status_of(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        const market_db::Row* r = find_row(id);
        return r ? r->status : -1;
    }
    int32_t quantity_of(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        const market_db::Row* r = find_row(id);
        return r ? r->l.item.quantity : -1;
    }
    int64_t copper_of(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        for (const svc::WalletEntry& e : purses[id]) {
            if (e.currency_guid == items::kCopperCurrency) return e.amount;
        }
        return 0;
    }
    int64_t collected_of(int64_t node) {
        std::lock_guard<std::mutex> lock(mu);
        return collected[node];
    }
    int64_t units_of(const std::string& id, int64_t record) {
        std::lock_guard<std::mutex> lock(mu);
        int64_t n = 0;
        for (const svc::InventoryItem& it : bags[id]) {
            if (it.item_record_id == record) n += it.quantity;
        }
        return n;
    }
    /// The mails of `owner` that carry money, newest last.
    std::vector<svc::Mail> sale_mails(const std::string& owner) {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<svc::Mail> out;
        for (const market_db::MailRow& m : mails) {
            if (m.owner == owner && m.mail.money > 0) out.push_back(m.mail);
        }
        return out;
    }
    std::vector<svc::Mail> mails_of(const std::string& owner) {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<svc::Mail> out;
        for (const market_db::MailRow& m : mails) {
            if (m.owner == owner) out.push_back(m.mail);
        }
        return out;
    }
    /// The character as a login reads it from the stored rows: its bags and purse.
    svc::Character stored_character(svc::Character base) {
        std::lock_guard<std::mutex> lock(mu);
        if (bags.count(base.id) != 0) base.items = bags[base.id];
        if (purses.count(base.id) != 0) base.wallet = purses[base.id];
        return base;
    }
    void store_character(const svc::Character& c) {
        std::lock_guard<std::mutex> lock(mu);
        bags[c.id] = c.items;
        purses[c.id] = c.wallet;
    }
    bool wait_started(int milliseconds) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return started; });
    }
    void release_all() {
        std::lock_guard<std::mutex> lock(mu);
        hold_buy = hold_remove = hold_expire = hold_claim = false;
        cv.notify_all();
    }
    void reset_started() {
        std::lock_guard<std::mutex> lock(mu);
        started = false;
    }

    // ---- the calls the world makes ---------------------------------------------------------------------------------------------------
    svc::MarketListingsResult list_market_listings() override {
        std::lock_guard<std::mutex> lock(mu);
        svc::MarketListingsResult r;
        r.status = svc::CallStatus::Ok;
        for (const market_db::Row& row : rows) {
            if (row.status != market_db::kListed) continue;
            svc::MarketListing l = row.l;
            l.seller_name = names[l.seller_id];
            r.listings.push_back(l);
        }
        for (const auto& [node, rate] : rates) r.node_taxes.push_back({node, rate, collected[node]});
        return r;
    }

    svc::MarketSaveResult save_inventory_and_listing_now(const std::string& id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet,
                                                         std::vector<svc::ProcessingJob>, std::vector<int64_t>, std::vector<svc::WarehouseItem>,
                                                         const svc::MarketListing& listing) override {
        std::lock_guard<std::mutex> lock(mu);
        ++listing_saves;
        if (find_row(listing.id)) return svc::MarketSaveResult{svc::CallStatus::Ok, std::string(), true};
        market_db::Row row;
        row.l = listing;
        row.l.seller_id = id;
        rows.push_back(row);
        bags[id] = std::move(items);
        purses[id] = std::move(wallet);
        return svc::MarketSaveResult{svc::CallStatus::Ok, std::string(), false};
    }

    void save_inventory(const std::string& id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                        std::vector<int64_t> known, std::vector<svc::WarehouseItem> warehouse) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            bags[id] = items;
            purses[id] = wallet;
        }
        RecordingSink::save_inventory(id, std::move(items), std::move(wallet), std::move(jobs), std::move(known));
        (void)warehouse;
    }

    svc::BuyResult buy_listing_now(const svc::BuyRequest& r) override {
        std::unique_lock<std::mutex> lock(mu);
        ++buy_calls;
        buys.push_back(r);
        started = true;
        cv.notify_all();
        cv.wait(lock, [this] { return !hold_buy; });
        svc::BuyResult out;
        out.status = svc::CallStatus::Ok;
        if (fail_buy_before > 0) {
            --fail_buy_before;
            out.status = svc::CallStatus::Unavailable;
            out.reason = "the service is down";
            return out;
        }
        market_db::Row* row = find_row(r.listing_id);
        const auto prev = purchases.find(r.purchase_id);
        if (prev != purchases.end()) {
            out.outcome = svc::MarketOutcome::Replayed;
            out.seller_id = row ? row->l.seller_id : std::string();
            if (lose_all_answers) {
                out = svc::BuyResult{};
                out.status = svc::CallStatus::Unavailable;
                out.reason = "the answer was lost";
            }
            return out;
        }
        const auto refuse = [&](svc::MarketOutcome o, int32_t available = 0) {
            out.outcome = o;
            out.available = available;
            return out;
        };
        if (!row) return refuse(svc::MarketOutcome::ListingGone);
        if (row->l.seller_id == r.character_id) return refuse(svc::MarketOutcome::OwnListing);
        if (row->status != market_db::kListed) return refuse(svc::MarketOutcome::ListingGone);
        if (row->l.expiry_date_ms > 0 && market_db::wall_ms() >= row->l.expiry_date_ms) return refuse(svc::MarketOutcome::ListingExpired);
        if (row->l.price_per_unit != r.price_per_unit) return refuse(svc::MarketOutcome::PriceChanged, row->l.item.quantity);
        if (r.quantity > row->l.item.quantity) return refuse(svc::MarketOutcome::Quantity, row->l.item.quantity);
        const float stored = rates.count(row->l.node) != 0 ? rates[row->l.node] : 0.5f;
        if (stored != r.tax_rate) return refuse(svc::MarketOutcome::RateChanged, row->l.item.quantity);
        // stored together
        bags[r.character_id] = r.bags.items;
        purses[r.character_id] = r.bags.wallet;
        const int64_t sum = row->l.price_per_unit * r.quantity;
        const std::string seller = row->l.seller_id;
        if (r.quantity == row->l.item.quantity) row->status = market_db::kSold;
        else row->l.item.quantity -= r.quantity;
        market_db::MailRow* sale = nullptr;
        for (market_db::MailRow& m : mails) {
            if (m.listing == row->l.id && !m.mail.money_claimed && (m.mail.status & svc::kMailStatusDeleted) == 0) sale = &m;
        }
        if (sale) {
            sale->mail.money += sum;
            sale->sold += r.quantity;
            sale->mail.status &= ~svc::kMailStatusRead;
            sale->mail.body = std::to_string(sale->sold) + " x " + r.item_name + " sold";
        } else {
            market_db::MailRow m;
            m.owner = seller;
            m.listing = row->l.id;
            m.sold = r.quantity;
            m.mail.id = fresh();
            m.mail.sender = "Marketplace";
            m.mail.subject = "Item sold";
            m.mail.body = std::to_string(r.quantity) + " x " + r.item_name + " sold";
            m.mail.money = sum;
            m.mail.issued_at_ms = market_db::wall_ms();
            mails.push_back(m);
            sale = &mails.back();
        }
        collected[row->l.node] += r.tax;
        if (rates.count(row->l.node) == 0) rates[row->l.node] = stored;
        purchases[r.purchase_id] = row->l.id;
        out.outcome = svc::MarketOutcome::Done;
        out.seller_id = seller;
        out.seller_mail = sale->mail;
        if (lose_buy_answers > 0 || lose_all_answers) {
            if (lose_buy_answers > 0) --lose_buy_answers;
            out = svc::BuyResult{};
            out.status = svc::CallStatus::Unavailable;
            out.reason = "the answer was lost";
        }
        return out;
    }

    svc::RemoveResult remove_listing_now(const svc::RemoveRequest& r) override {
        std::unique_lock<std::mutex> lock(mu);
        ++remove_calls;
        removes.push_back(r);
        started = true;
        cv.notify_all();
        cv.wait(lock, [this] { return !hold_remove; });
        svc::RemoveResult out;
        out.status = svc::CallStatus::Ok;
        market_db::Row* row = find_row(r.listing_id);
        if (!row || row->l.seller_id != r.character_id) {
            out.outcome = svc::MarketOutcome::ListingGone;
            return out;
        }
        if (row->status == market_db::kRemoved) {
            out.outcome = svc::MarketOutcome::Replayed;
            return out;
        }
        if (row->status != market_db::kListed) {
            out.outcome = svc::MarketOutcome::ListingGone;
            return out;
        }
        if (row->l.expiry_date_ms > 0 && market_db::wall_ms() >= row->l.expiry_date_ms) {
            out.outcome = svc::MarketOutcome::ListingExpired;
            return out;
        }
        if (row->l.item.quantity != r.expected_quantity) {
            out.outcome = svc::MarketOutcome::Quantity;
            out.available = row->l.item.quantity;
            return out;
        }
        bags[r.character_id] = r.bags.items;
        purses[r.character_id] = r.bags.wallet;
        row->status = market_db::kRemoved;
        out.outcome = svc::MarketOutcome::Done;
        if (lose_remove_answers > 0) {
            --lose_remove_answers;
            out = svc::RemoveResult{};
            out.status = svc::CallStatus::Unavailable;
            out.reason = "the answer was lost";
        }
        return out;
    }

    svc::ExpireResult expire_listing_now(const std::string& id, const std::string& name) override {
        std::unique_lock<std::mutex> lock(mu);
        ++expire_calls;
        expired_ids.push_back(id);
        started = true;
        cv.notify_all();
        cv.wait(lock, [this] { return !hold_expire; });
        svc::ExpireResult out;
        out.status = svc::CallStatus::Ok;
        if (fail_expire_before > 0) {
            --fail_expire_before;
            out.status = svc::CallStatus::Unavailable;
            return out;
        }
        market_db::Row* row = find_row(id);
        if (!row) {
            out.outcome = svc::MarketOutcome::ListingGone;
            return out;
        }
        if (row->status == market_db::kExpiredClaimed) {
            out.outcome = svc::MarketOutcome::Replayed;
            out.seller_id = row->l.seller_id;
            return out;
        }
        if (row->status != market_db::kListed) {
            out.outcome = svc::MarketOutcome::ListingGone;
            return out;
        }
        if (!(row->l.expiry_date_ms > 0 && market_db::wall_ms() >= row->l.expiry_date_ms)) {
            out.outcome = svc::MarketOutcome::NotExpired;
            return out;
        }
        row->status = market_db::kExpiredClaimed;
        market_db::MailRow m;
        m.owner = row->l.seller_id;
        m.mail.id = fresh();
        m.mail.sender = "Marketplace";
        m.mail.subject = "Listing expired";
        m.mail.body = "Your listing of " + std::to_string(row->l.item.quantity) + " x " + name + " ended after 3 days. The item is attached.";
        m.mail.issued_at_ms = market_db::wall_ms();
        svc::MailItem item;
        item.item = row->l.item;
        m.mail.items.push_back(item);
        mails.push_back(m);
        out.outcome = svc::MarketOutcome::Done;
        out.seller_id = row->l.seller_id;
        out.seller_mail = m.mail;
        return out;
    }

    // ---- mail ----------------------------------------------------------------------------------------------------------------------------
    svc::MailsResult load_mails(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu);
        svc::MailsResult r;
        r.status = svc::CallStatus::Ok;
        for (auto it = mails.rbegin(); it != mails.rend(); ++it) {
            if (it->owner == id && (it->mail.status & svc::kMailStatusDeleted) == 0) r.mails.push_back(it->mail);
        }
        return r;
    }
    svc::MailSaveResult save_mail(const std::string& id, const svc::Mail& mail) override {
        std::lock_guard<std::mutex> lock(mu);
        for (market_db::MailRow& m : mails) {
            if (m.owner == id && m.mail.id == mail.id) {
                m.mail.status |= mail.status;
                return svc::MailSaveResult{svc::CallStatus::Ok, std::string()};
            }
        }
        return svc::MailSaveResult{svc::CallStatus::NotFound, "no such mail"};
    }
    /// The claim of items and of money: ONE call that stores the bags, the purse and the claimed marks together, or refuses a mail that is claimed already, or whose money is another
    /// amount than the one the claim credited (a sale was added to the mail meanwhile).
    svc::MailClaimResult claim_mail(const std::string& id, const std::vector<svc::MailClaim>& claims, const std::vector<svc::MoneyClaim>& money_mails, std::vector<svc::InventoryItem> items,
                                    std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob>, std::vector<int64_t>, std::vector<svc::WarehouseItem>) override {
        std::unique_lock<std::mutex> lock(mu);
        ++claim_calls;
        started = true;
        cv.notify_all();
        cv.wait(lock, [this] { return !hold_claim; });
        svc::MailClaimResult r;
        r.status = svc::CallStatus::Ok;
        size_t already = 0;
        ++claim_money_asks;
        for (const svc::MoneyClaim& money : money_mails) {
            market_db::MailRow* row = nullptr;
            for (market_db::MailRow& m : mails) {
                if (m.owner == id && m.mail.id == money.mail_id) row = &m;
            }
            if (!row || row->mail.money <= 0) {
                r.status = svc::CallStatus::NotFound;
                return r;
            }
            if (row->mail.money != money.copper) {
                ++claim_changed_refusals;   // a sale was added to the mail after the world read it: nothing is written
                r.status = svc::CallStatus::Refused;
                r.claimed_before = true;
                return r;
            }
            if (row->mail.money_claimed) ++already;
        }
        for (const svc::MailClaim& c : claims) {
            market_db::MailRow* row = nullptr;
            for (market_db::MailRow& m : mails) {
                if (m.owner == id && m.mail.id == c.mail_id) row = &m;
            }
            bool found = false;
            if (row) {
                for (svc::MailItem& it : row->mail.items) {
                    if (it.item.instance_guid == c.instance_guid) {
                        found = true;
                        if (it.claimed) ++already;
                    }
                }
            }
            if (!found) {
                r.status = svc::CallStatus::NotFound;
                return r;
            }
        }
        if (already > 0) {
            // a repeat of the claim that did it: the stored bags are the request's and, with money, so is the purse (any other purse is a second claim of money that was taken)
            if (already == claims.size() + money_mails.size() && bags[id].size() == items.size() && (money_mails.empty() || purses[id] == wallet)) {
                r.replayed = true;
                return r;
            }
            r.status = svc::CallStatus::Refused;
            r.claimed_before = true;
            return r;
        }
        bags[id] = std::move(items);
        purses[id] = std::move(wallet);
        for (const svc::MoneyClaim& money : money_mails) {
            for (market_db::MailRow& m : mails) {
                if (m.owner == id && m.mail.id == money.mail_id) m.mail.money_claimed = true;
            }
        }
        for (const svc::MailClaim& c : claims) {
            for (market_db::MailRow& m : mails) {
                if (m.owner != id || m.mail.id != c.mail_id) continue;
                for (svc::MailItem& it : m.mail.items) {
                    if (it.item.instance_guid == c.instance_guid) it.claimed = true;
                }
            }
        }
        r.claimed = static_cast<int32_t>(claims.size() + money_mails.size());
        return r;
    }
};
