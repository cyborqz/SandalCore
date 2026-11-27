// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/market_store.h
//
//  ServiceMarketStore: the world's listings over the character service's two market calls.  The storefronts, a seller's
//  listings, the cap and a lookup read the memory indexes of market/listings.h, which this store fills with every listed row
//  when the world gets its sink and with each listing when its save answers Ok.  The saves run on one worker thread
//  (MarketJobs): a listing and the seller's bags and purse are written in one call, off the network thread, and the session
//  takes the answer on a later tick.
//
//  The worker settles every save: an answer that is not clear (the character service was unavailable, or failed) may mean the
//  listing was written, so the same save is asked for again, with a growing pause, until the answer is clear.  The character
//  service answers a listing it already holds as saved.  A world that stops with a save still undecided says so.
//
//  A world whose listings could not be read refuses registrations and shows no storefront; the worker reads them again until
//  it succeeds, and the network thread takes the rows from the worker (poll), never reading them while the worker fills them.
//
//  A session that ends while its saves are in flight hands them to the store with the bags it ends with.  The world's tick
//  (poll) takes their answers: Ok joins the listing to the cache, a refusal gives back what the listing took (give_back.h), and
//  then the bags are written through the ordinary saver, after the listing.
// ============================================================================
#pragma once

#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "items/wallet.h"
#include "market/give_back.h"
#include "market/listings.h"
#include "session/position_saver.h"
#include "svc/services.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace aoc::worldd::market {

/// One worker thread for a world's market calls: the character service's call may take seconds, and the network thread only submits it and takes the answer on a later tick.
/// The calls go in the order they were submitted.  A call the service does not answer clearly is not tried again in a loop: it goes to the back of the queue with a pause that
/// grows to a cap, so one stuck call never keeps the calls of other characters waiting.  A seller has at most one save in flight at a time (the session refuses a second
/// listing, and the world refuses one while an ended session's save is unfinished), so a seller's saves still go in the order they were made.
class MarketJobs {
public:
    MarketJobs() : thread_([this] { run(); }) {}
    ~MarketJobs() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
            ++changed_;
            for (const Job& job : jobs_) {
                if (job.load) continue;
                if (job.attempt == 0) log_undecided(job.save, "it was never sent, so the stack stays in the seller's saved bags");
                else log_undecided(job.save, "the character service may or may not hold it");
            }
            jobs_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    MarketJobs(const MarketJobs&) = delete;
    MarketJobs& operator=(const MarketJobs&) = delete;

    /// Queues a listing's save.  Its answer is a clear one: the worker asks again until it has one.
    uint64_t submit(ListingSave save, PositionSink* sink) {
        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t ticket = next_++;
        if (stop_) {
            done_[ticket] = ListingSaveAnswer{svc::CallStatus::Refused, "the market worker is stopping"};
            return ticket;
        }
        jobs_.push_back(Job{ticket, false, std::move(save), sink});
        ++changed_;
        cv_.notify_one();
        return ticket;
    }

    /// Queues a read of every listed row, which the worker asks for again until it answers Ok.
    uint64_t submit_load(PositionSink* sink) {
        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t ticket = next_++;
        if (!stop_) {
            jobs_.push_back(Job{ticket, true, ListingSave{}, sink});
            ++changed_;
            cv_.notify_one();
        }
        return ticket;
    }

    bool take(uint64_t ticket, ListingSaveAnswer& out) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = done_.find(ticket);
        if (it == done_.end()) return false;
        out = std::move(it->second);
        done_.erase(it);
        return true;
    }

    bool take_load(uint64_t ticket, svc::MarketListingsResult& out) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = loads_.find(ticket);
        if (it == loads_.end()) return false;
        out = std::move(it->second);
        loads_.erase(it);
        return true;
    }

    /// Drops a ticket and its answer; a read nobody waits for any more is not asked for again.
    void forget(uint64_t ticket) {
        std::lock_guard<std::mutex> lock(mu_);
        done_.erase(ticket);
        loads_.erase(ticket);
        forgotten_.insert(ticket);
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Job {
        uint64_t       ticket = 0;
        bool           load = false;   // a read of the listings; otherwise a listing's save
        ListingSave    save;
        PositionSink*  sink = nullptr;
        int            attempt = 0;    // the tries that were not answered clearly
        Clock::time_point not_before{};   // the next try waits for this
    };

    /// The pause before try n + 1 is n times the sink's retry delay, up to this many.
    static constexpr int kLongestPause = 5;

    static void log_undecided(const ListingSave& save, const char* why) {
        spdlog::error("[market] stopping with listing {} undecided: seller {} ({}), stack {} x{} of item {}, fee {} copper; {}", items::guid_hex(save.listing.guid),
                      items::guid_hex(save.listing.seller), save.listing.seller_name, save.listing.item.instance_guid, save.listing.item.quantity,
                      save.listing.item.item_record_id, save.fee, why);
    }

    /// Runs the first call whose time has come, one try at a time; a call that was not answered clearly goes to the back of the queue, to be tried again after its pause.
    void run() {
        std::unique_lock<std::mutex> lock(mu_);
        for (;;) {
            if (stop_) return;
            const Clock::time_point now = Clock::now();
            auto due = jobs_.end();
            Clock::time_point soonest = Clock::time_point::max();
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                if (it->not_before <= now) {
                    due = it;
                    break;
                }
                soonest = std::min(soonest, it->not_before);
            }
            if (due == jobs_.end()) {
                const uint64_t seen = changed_;
                if (jobs_.empty()) cv_.wait(lock, [&] { return stop_ || changed_ != seen; });
                else cv_.wait_until(lock, soonest, [&] { return stop_ || changed_ != seen; });
                continue;
            }
            Job job = std::move(*due);
            jobs_.erase(due);
            lock.unlock();
            const bool again = job.load ? load_once(job) : save_once_and_settle(job);
            lock.lock();
            if (!again) continue;
            if (stop_) {   // the world stopped while the call was undecided
                if (!job.load) log_undecided(job.save, "the character service may or may not hold it");
                return;
            }
            ++job.attempt;
            job.not_before = Clock::now() + pause_of(job.sink, job.attempt);
            jobs_.push_back(std::move(job));
        }
    }

    /// The pause before the next try of a call whose `attempt`-th try was not answered clearly.
    static std::chrono::milliseconds pause_of(const PositionSink* sink, int attempt) {
        const std::chrono::milliseconds step = sink ? sink->retry_delay() : std::chrono::milliseconds(1000);
        return step * std::min(attempt, kLongestPause);
    }

    /// One try of a listing's save: the same listing, the same bags and the same purse every time.
    static ListingSaveAnswer save_once(const Job& job) {
        ListingSaveAnswer answer;
        if (!job.sink) {
            answer.status = svc::CallStatus::Refused;   // nothing can have been written
            answer.reason = "the world has no character service";
            return answer;
        }
        svc::MarketListing listing;
        listing.id = items::guid_hex(job.save.listing.guid);
        listing.seller_id = items::guid_hex(job.save.listing.seller);
        listing.seller_name = job.save.listing.seller_name;
        listing.node = job.save.listing.node;
        listing.item = job.save.listing.item;
        listing.price_per_unit = job.save.listing.price_per_unit;
        listing.status = job.save.listing.status;
        listing.issue_date_ms = job.save.issue_date_ms;
        listing.expiry_date_ms = job.save.expiry_date_ms;
        const svc::MarketSaveResult saved = job.sink->save_inventory_and_listing_now(listing.seller_id, job.save.items, job.save.wallet, job.save.jobs, job.save.recipes,
                                                                                     job.save.warehouse, listing);
        answer.status = saved.status;
        answer.reason = saved.reason;
        return answer;
    }

    /// One try of a listing's save.  True when the answer was not clear: the outcome stays unknown, nothing is given back, and the save is asked for again.
    bool save_once_and_settle(Job& job) {
        ListingSaveAnswer answer = save_once(job);
        if (is_clear(answer.status)) {
            std::lock_guard<std::mutex> lock(mu_);
            if (forgotten_.erase(job.ticket) == 0) done_[job.ticket] = std::move(answer);
            return false;
        }
        if (job.attempt == 0 || (job.attempt + 1) % 12 == 0) {   // (a service that stays down is said so once a minute or so, not at every try)
            spdlog::warn("[market] the save of listing {} was not answered clearly ({}: {}): it is asked for again (try {} was this one)", items::guid_hex(job.save.listing.guid),
                         svc::call_status_name(answer.status), answer.reason, job.attempt + 1);
        }
        return true;
    }

    /// One try of the read of every listed row.  True when it failed: the read is asked for again.
    bool load_once(Job& job) {
        svc::MarketListingsResult result;
        if (job.sink) {
            result = job.sink->list_market_listings();
        } else {
            result.status = svc::CallStatus::Failed;
            result.reason = "the world has no character service";
        }
        if (result.status == svc::CallStatus::Ok) {
            std::lock_guard<std::mutex> lock(mu_);
            if (forgotten_.erase(job.ticket) == 0) loads_[job.ticket] = std::move(result);
            return false;
        }
        if (job.attempt == 0 || (job.attempt + 1) % 12 == 0) {
            spdlog::warn("[market] the saved listings were not read ({}: {}): the read is tried again (try {} was this one)", svc::call_status_name(result.status), result.reason,
                         job.attempt + 1);
        }
        std::lock_guard<std::mutex> lock(mu_);
        return forgotten_.erase(job.ticket) == 0;   // a newer read took its place: this one is dropped
    }

    std::mutex                              mu_;
    std::condition_variable                 cv_;
    std::deque<Job>                         jobs_;
    std::map<uint64_t, ListingSaveAnswer>   done_;
    std::map<uint64_t, svc::MarketListingsResult> loads_;
    std::set<uint64_t>                      forgotten_;
    bool                                    stop_ = false;
    uint64_t                                changed_ = 0;   // counts every change of the queue: the worker waits for it to move
    uint64_t                                next_ = 1;
    std::thread                             thread_;   // last: it starts once every other member is built
};

/// The world's listings over the character service.  The memory indexes are the cache the window reads; every save is submitted to the worker, and a listing joins the cache
/// when the answer to its save is taken, and only when it is Ok.
class ServiceMarketStore final : public MarketStore {
public:
    ServiceMarketStore() = default;
    ~ServiceMarketStore() override = default;

    /// The world's sink, the content database the item names come from, and the world's time now (seconds since the server started).  Reads every listed row when the world
    /// gets its sink; a failed read leaves the store not loaded, so no registration is made and no storefront answers until the worker's later read succeeds (poll takes its
    /// rows).  A later call with the same sink does not read again.
    void attach(PositionSink* sink, const content::ContentDb* content, double world_now) {
        if (!sink) return;
        const bool same = sink == sink_ && loaded_;
        sink_ = sink;
        content_ = content;
        if (same) return;   // one read per sink: a test that swaps the sink asks for the rows of the new one
        loaded_ = false;
        if (load_ticket_ != 0 && jobs_) jobs_->forget(load_ticket_);   // a read for the sink before is not wanted any more
        load_ticket_ = 0;
        const svc::MarketListingsResult result = sink->list_market_listings();
        if (result.status != svc::CallStatus::Ok) {
            spdlog::warn("[market] the saved listings were not read ({}: {}): no registration and no storefront until they are; the read is tried again", svc::call_status_name(result.status),
                         result.reason);
            load_ticket_ = jobs().submit_load(sink);
            return;
        }
        fill(result, world_now);
    }

    bool loaded() const override { return loaded_; }

    void add(const Listing& listing) override { cache_.add(listing); }
    std::vector<Storefront> storefronts(int64_t node) const override { return cache_.storefronts(node); }
    std::vector<Listing> listings(int64_t node, const ::aoc::protocol::repl::GuidValue& seller) const override { return cache_.listings(node, seller); }
    size_t count(const ::aoc::protocol::repl::GuidValue& seller) const override { return cache_.count(seller); }
    const Listing* find(const ::aoc::protocol::repl::GuidValue& guid) const override { return cache_.find(guid); }
    size_t size() const override { return cache_.size(); }

    /// Says whether a log-in of a character has marked it and is waiting to be applied (PlayerWorld::login_pending).  A listing of such a character is not saved: the log-in
    /// waits for the saves the world holds (PlayerWorld::character_writes_pending) and then reads the bags, so a save that began after that look would be written under the bags
    /// the log-in read.  Set once by the world; asked on the network thread.
    void set_login_check(std::function<bool(const std::string& character_id)> pending) { login_pending_ = std::move(pending); }

    /// Queues one listing's save on the worker; its ticket names the answer.  0: the store cannot save it (no sink, the listings not read, or a log-in of the seller waits).
    /// The save is counted BEFORE the log-in mark is looked at, and the log-in marks BEFORE it looks at the count, so one of them always sees the other: the log-in finds this save
    /// (it waits), or this call finds the mark (it saves nothing).
    uint64_t submit_listing(ListingSave save) override {
        if (!sink_ || !loaded_) return 0;
        const Listing listing = save.listing;
        const std::string seller = items::guid_hex(listing.seller);
        hold(seller, 1);
        if (login_pending_ && login_pending_(seller)) {
            release(seller, 1);
            return 0;
        }
        const uint64_t ticket = jobs().submit(std::move(save), sink_);
        submitted_[ticket] = listing;
        return ticket;
    }
    bool take_listing(uint64_t ticket, ListingSaveAnswer& out) override {
        std::string character;
        if (!take_one(ticket, out, &character)) return false;
        release(character, 1);
        return true;
    }
    void forget_listing(uint64_t ticket) override {
        const auto it = submitted_.find(ticket);
        if (it != submitted_.end()) {
            release(items::guid_hex(it->second.seller), 1);
            submitted_.erase(it);
        }
        if (jobs_) jobs_->forget(ticket);
    }

    void hand_over(HandOver over) override {
        if (over.listings.empty()) return;
        Orphan orphan;
        orphan.character_id = std::move(over.character_id);
        orphan.seller = over.seller;
        orphan.listings = std::move(over.listings);
        orphan.held = orphan.listings.size();   // their saves stay counted until the bags are written
        orphan.bags = std::move(over.bags);
        orphans_.push_back(std::move(orphan));
    }
    bool busy(const ::aoc::protocol::repl::GuidValue& seller) const override {
        return std::any_of(orphans_.begin(), orphans_.end(), [&](const Orphan& o) { return o.seller == seller; });
    }
    /// A character's saves that have no answer taken, and the bags of a session that ended with some not queued yet: counted when a save is submitted and released when its
    /// answer is taken (a session's own) or when the bags are queued in the sink (an ended session's: poll releases after write_bags, so the sink's own busy check sees the write from
    /// then on).  The log-in thread reads this (PlayerWorld::character_writes_pending, first of its three checks), so the counts have their own lock.
    bool saving(const std::string& character_id) const override {
        std::lock_guard<std::mutex> lock(gate_mu_);
        const auto it = open_saves_.find(character_id);
        return it != open_saves_.end() && it->second > 0;
    }

    /// The world's own step (PlayerWorld::tick_world, at the world's time `world_now`): takes the rows a failed first read was asked for again, and the answers of the saves
    /// whose session ended.  With every answer of an ended session taken, its bags are written through the sink: after the listings, with the units and the fee given back for
    /// a refused one.
    void poll(const items::ItemCatalog* catalog, double world_now) {
        take_loaded(world_now);
        for (size_t i = 0; i < orphans_.size();) {
            Orphan& orphan = orphans_[i];
            while (!orphan.listings.empty()) {
                ListingSaveAnswer answer;
                if (!take_one(orphan.listings.front().ticket, answer)) break;
                if (answer.status != svc::CallStatus::Ok) {
                    spdlog::info("[market] {}: a listing's save was refused after its session ended ({}: {}): the units and the fee go back into the bags", orphan.listings.front().listing.seller_name,
                                 svc::call_status_name(answer.status), answer.reason);
                    orphan.refused.push_back(orphan.listings.front());
                }
                orphan.listings.erase(orphan.listings.begin());
            }
            if (!orphan.listings.empty()) {
                ++i;
                continue;
            }
            write_bags(orphan, catalog);
            release(orphan.character_id, orphan.held);
            orphans_.erase(orphans_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    /// When the world stops: polls until every ended session's saves are finished and its bags written, or `max_wait` has passed.
    void settle(std::chrono::milliseconds max_wait, const items::ItemCatalog* catalog, double world_now) {
        const auto end = std::chrono::steady_clock::now() + max_wait;
        for (;;) {
            poll(catalog, world_now);
            if (orphans_.empty() || std::chrono::steady_clock::now() >= end) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

private:
    /// The saves of a session that ended, and the bags it ended with.
    struct Orphan {
        std::string                      character_id;
        ::aoc::protocol::repl::GuidValue seller;
        std::vector<PendingListing>      listings;   // not answered yet, oldest first
        std::vector<PendingListing>      refused;    // answered with a refusal: to give back
        BagSnapshot                      bags;
        size_t                           held = 0;   // the saves this ended session still has counted in open_saves_
    };

    MarketJobs& jobs() {
        if (!jobs_) jobs_ = std::make_unique<MarketJobs>();
        return *jobs_;
    }

    /// Takes a save's answer; an Ok answer joins the listing to the cache at once.  `character` gets the seller's id (empty for an unknown ticket).
    bool take_one(uint64_t ticket, ListingSaveAnswer& out, std::string* character = nullptr) {
        if (!jobs_ || !jobs_->take(ticket, out)) return false;
        const auto it = submitted_.find(ticket);
        if (it != submitted_.end()) {
            if (character) *character = items::guid_hex(it->second.seller);
            if (out.status == svc::CallStatus::Ok) cache_.add(it->second);
            submitted_.erase(it);
        }
        return true;
    }

    void hold(const std::string& character, size_t n) {
        std::lock_guard<std::mutex> lock(gate_mu_);
        open_saves_[character] += static_cast<int>(n);
    }
    void release(const std::string& character, size_t n) {
        if (character.empty() || n == 0) return;
        std::lock_guard<std::mutex> lock(gate_mu_);
        const auto it = open_saves_.find(character);
        if (it == open_saves_.end()) return;
        it->second -= static_cast<int>(n);
        if (it->second <= 0) open_saves_.erase(it);
    }

    void take_loaded(double world_now) {
        if (load_ticket_ == 0 || !jobs_) return;
        svc::MarketListingsResult result;
        if (!jobs_->take_load(load_ticket_, result)) return;
        load_ticket_ = 0;
        fill(result, world_now);
    }

    /// Writes the bags of an ended session through the sink, once the answers of its saves are in; a refused save's units and fee are put back first.
    void write_bags(Orphan& orphan, const items::ItemCatalog* catalog) {
        BagSnapshot& bags = orphan.bags;
        if (!orphan.refused.empty()) {
            const NoItemCatalog none;
            const items::ItemCatalog& known = catalog ? *catalog : static_cast<const items::ItemCatalog&>(none);
            std::string error;
            std::optional<items::Inventory> inventory = items::Inventory::from_items(bags.items, error, catalog);
            if (!inventory) {
                spdlog::error("[market] {}: the bags of the ended session do not read ({}): the units of {} refused listing(s) are not given back", orphan.character_id, error,
                              orphan.refused.size());
            } else {
                items::Wallet wallet(bags.wallet);
                for (const PendingListing& job : orphan.refused) {
                    if (!give_back(*inventory, wallet, known, job)) {
                        spdlog::error("[market] {}: the stack {} x{} found no place in the bags", orphan.character_id, job.taken.instance_guid, job.quantity);
                    }
                }
                bags.items = inventory->items();
                bags.wallet = wallet.entries();
            }
        }
        if (sink_) {
            sink_->save_inventory(orphan.character_id, std::move(bags.items), std::move(bags.wallet), std::move(bags.jobs), std::move(bags.recipes), std::move(bags.warehouse));
        }
    }

    /// Fills the cache from the rows the service sent (the whole of them, replacing what the cache held) and marks the store loaded.  `world_now` is the world's time at this
    /// moment: the client's synchronized clock reads effect_clock(world_now) now, and a saved Unix time is that much before or after it.
    void fill(const svc::MarketListingsResult& result, double world_now) {
        const int64_t unix_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        cache_.clear();
        for (const svc::MarketListing& row : result.listings) {
            const std::optional<::aoc::protocol::repl::GuidValue> seller = items::guid_of(row.seller_id);
            const std::optional<::aoc::protocol::repl::GuidValue> guid = items::guid_of(row.id);
            if (!seller || !guid) {
                spdlog::warn("[market] a saved listing {} has a seller or a listing guid that does not read: left out", row.id);
                continue;
            }
            Listing listing;
            listing.node = row.node;
            listing.seller = *seller;
            listing.seller_name = row.seller_name;
            listing.guid = *guid;
            listing.item = row.item;
            listing.item.container = svc::ItemContainer::Backpack;
            listing.item_name = item_name(row.item.item_record_id, content_);
            listing.price_per_unit = row.price_per_unit;
            // the database's time is Unix milliseconds; the client reads its synchronized clock, whose offset from Unix time is fixed while the world runs
            listing.issue_date = bootstrap::effect_clock(world_now) + (static_cast<double>(row.issue_date_ms) - static_cast<double>(unix_now_ms)) / 1000.0;
            listing.expiration_date = bootstrap::effect_clock(world_now) + (static_cast<double>(row.expiry_date_ms) - static_cast<double>(unix_now_ms)) / 1000.0;
            listing.status = static_cast<uint8_t>(row.status);
            cache_.add(listing);
        }
        loaded_ = true;
        spdlog::info("[market] {} saved listing(s) read", cache_.size());
    }

    static std::string item_name(int64_t record_id, const content::ContentDb* content) {
        if (!content) return {};
        std::string error;
        const std::optional<content::ItemRecord> record = content->item(record_id, error);
        if (!record) return {};
        return record->display_name.empty() ? record->name : record->display_name;
    }

    PositionSink*                  sink_ = nullptr;
    const content::ContentDb*      content_ = nullptr;
    bool                           loaded_ = false;
    MemoryMarketStore              cache_;
    std::map<uint64_t, Listing>    submitted_;    // the listing of each save the worker has not answered, or the session has not taken
    std::vector<Orphan>            orphans_;      // the saves of sessions that ended, oldest first
    mutable std::mutex             gate_mu_;      // guards open_saves_, which the log-in thread reads
    std::map<std::string, int>     open_saves_;   // by character id: the saves with no answer taken, and the saves of ended sessions whose bags are not queued
    std::function<bool(const std::string&)> login_pending_;   // a log-in of the character waits to be applied (see set_login_check)
    uint64_t                       load_ticket_ = 0;
    std::unique_ptr<MarketJobs>    jobs_;
};

}  // namespace aoc::worldd::market
