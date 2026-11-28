// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/position_saver.h
//
//  Saving a player's position (plan A6, spec section 10) and, since plan B2, its inventory: the character service's
//  SaveCharacterState and SaveInventory calls, off the network thread.
//
//  PositionSink is what a PlayerSession hands a position or an inventory to: save() and save_inventory() return at once and
//  never touch the network.  CharacterSaver is the sink worldd runs: one worker thread with a bounded queue that calls
//  CharacterApi::save_position and save_inventory.  Latest wins: the queue holds at most one entry per character, with at most one
//  position and one inventory in it (one saved while an older one waits replaces it, in the older one's place), so a client that
//  moves for an hour costs one call per interval however slow the service is, and a service that is down for a minute does not
//  pile up positions.  A save the service could not take now (Unavailable) is tried again, up to max_attempts times, with a growing
//  pause, each part (the position, the inventory) on its own count and unless a newer one came in meanwhile; a refusal (NotFound, an
//  invalid position, items the service's rules refuse) is logged and dropped.  When more than max_queued characters wait the oldest
//  entry is dropped and the loss logged.
//
//  Shutdown: flush(max_wait) waits until everything queued is saved or the time is up and says which; the destructor flushes
//  briefly, then abandons what is left (logged) and joins the worker, which finishes the call it is in (CharacterApi's own deadline
//  bounds it: 2 s for GrpcServices::save_position and save_inventory).  worldd's main sets destructor_wait to 0 and flushes once
//  itself (5 s), so its worst case from the signal is 0.5 s (the clients are told) + 5 s + 2 s = 7.5 s (a character's position and
//  inventory are two calls, so the call in flight at the end may be the second of two: the bound is the same, the flush waits for
//  both).  The network thread only ever queues: it is never in this path.
// ============================================================================
#pragma once

#include "svc/services.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
// (C O A | Cyb)

namespace aoc { namespace worldd {

/// Where a session hands the positions and inventories it wants saved.
class PositionSink {
public:
    virtual ~PositionSink() = default;
    /// Queues `position` for `character_id` (32 hex digits, as the character store has it). Never blocks on the network.
    virtual void save(const std::string& character_id, const svc::Position& position) = 0;
    /// Plan B2: queues the character's whole inventory (what SaveInventory replaces its items with). Never blocks on the network.
    /// A sink that does not save inventories leaves the default, which drops them.
    virtual void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items) {
        (void)character_id;
        (void)items;
    }
    /// The vendors milestone, step 0: the inventory with the character's wallet (stored in the same call). A sink that does not keep wallets saves the items alone.
    virtual void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet) {
        (void)wallet;
        save_inventory(character_id, std::move(items));
    }
    /// Crafting, step 1: the inventory with the wallet and the character's processing jobs (stored in the same call). A sink that does not keep jobs saves the items and the wallet.
    virtual void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs) {
        (void)jobs;
        save_inventory(character_id, std::move(items), std::move(wallet));
    }
    /// Crafting, step 3: the same with the recipes the character was taught (stored in the same call).  A sink that does not keep them saves the rest.
    virtual void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                std::vector<int64_t> known_recipes) {
        (void)known_recipes;
        save_inventory(character_id, std::move(items), std::move(wallet), std::move(jobs));
    }
    /// Storage clerks, step 0: the same with the character's warehouse (stored in the same call).  A sink that does not keep it saves the rest.
    virtual void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse) {
        (void)warehouse;
        save_inventory(character_id, std::move(items), std::move(wallet), std::move(jobs), std::move(known_recipes));
    }
    /// Plan D1: queues the character's progress (SaveProgress: level, class, XP rows). Never blocks on the network.
    virtual void save_progress(const std::string& character_id, svc::Progress progress) {
        (void)character_id;
        (void)progress;
    }
    /// Social, step 9: queues a whole-guild save (SaveGuild: a create when its version is 0).  Never blocks on the network; the latest save of one guild wins.
    virtual void save_guild(const svc::Guild& guild) { (void)guild; }
    /// Social, step 9: queues the character's join cooldown (Unix milliseconds; 0 clears it).  Never blocks on the network.
    virtual void save_guild_cooldown(const std::string& character_id, int64_t until_ms) {
        (void)character_id;
        (void)until_ms;
    }
    /// Social, step 9: writes a new guild (a create: version 0, with its leader as the only member) to the character service at once and answers what the service
    /// said, so a name that another guild has (a guild none of whose members is online here, which this world does not know) is refused before the guild exists.
    /// Called from the network thread, once for each create.  Ok: the service holds the guild at version 1 and the guild needs no create save.  Refused: the name is
    /// taken.  A sink that cannot ask (a test's, or one whose service does not answer) answers Unavailable: the guild is made here and saved with save_guild.
    virtual svc::CallStatus create_guild_now(const svc::Guild& guild) {
        (void)guild;
        return svc::CallStatus::Unavailable;
    }
    /// Mail: the character's live mails with their items and status, newest first (the character service's order).  Blocks; the caller may be
    /// the network thread, as with create_guild_now.  A sink that cannot ask answers Failed.
    virtual svc::MailsResult load_mails(const std::string& character_id) {
        (void)character_id;
        return {};
    }
    /// Mail: writes one mail whole (its content, status and each item's claimed flag).  Blocks; the caller may be the network thread.
    /// A sink that cannot ask answers Failed.
    virtual svc::MailSaveResult save_mail(const std::string& character_id, const svc::Mail& mail) {
        (void)character_id;
        (void)mail;
        return {};
    }
    /// Mail: the admin send; the store gives the mail its id, issue time and each attachment its instance guid and creation time.  Blocks.
    /// A sink that cannot ask answers Failed.
    virtual svc::MailResult create_mail(const std::string& character_id, const svc::NewMail& mail) {
        (void)character_id;
        (void)mail;
        return {};
    }
    /// Mail: the claim.  Stores the bags and marks the attachments claimed in ONE call of the character service, so the item is in the stored bags exactly when its mail says
    /// claimed.  Blocks; the caller is the mail worker.  Ok: both stored (or an earlier identical claim already did).  Refused, NotFound: nothing was stored and nothing will be.
    /// Unavailable, Failed: unknown.  A sink that cannot ask answers Unavailable.
    virtual svc::MailClaimResult claim_mail_items(const std::string& character_id, const std::vector<svc::MailClaim>& claims, std::vector<svc::InventoryItem> items,
                                                  std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs, std::vector<int64_t> known_recipes,
                                                  std::vector<svc::WarehouseItem> warehouse) {
        (void)character_id;
        (void)claims;
        (void)items;
        (void)wallet;
        (void)jobs;
        (void)known_recipes;
        (void)warehouse;
        svc::MailClaimResult result;
        result.status = svc::CallStatus::Unavailable;
        return result;
    }
    /// Whether a write of the character is still queued or running in this sink (its position, bags, progress, friend lists).  The log-in waits for false before it reads the
    /// character (login/login_service.h), so it never reads state a write can still change.  Safe to ask from any thread.  A sink that keeps nothing (a test's) answers false.
    virtual bool character_busy(const std::string& character_id) const {
        (void)character_id;
        return false;
    }
    /// Marketplace: the character service's two market calls.  The load reads every listed row with its seller's name; the save writes the bags, the purse and
    /// the rest with one listing in one transaction, answering after the store has it.  Both block; the load is made when the world gets its sink (and again off the
    /// network thread when it failed), the save on the world's market worker, which asks for the same save again until the answer is clear (Ok, already saved, or a
    /// refusal).  A sink that keeps no listings answers an empty load and a save it cannot make Unavailable.
    virtual svc::MarketListingsResult list_market_listings() { return svc::MarketListingsResult{svc::CallStatus::Ok, std::string(), {}}; }
    /// Marketplace: the pause before the world asks for a market call again after an answer that was not clear: n times this before try n + 1, as the saver's own retries.
    virtual std::chrono::milliseconds retry_delay() const { return std::chrono::milliseconds(1000); }
    virtual svc::MarketSaveResult save_inventory_and_listing_now(const std::string& character_id, std::vector<svc::InventoryItem> items,
                                                                 std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                                                 std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse,
                                                                 const svc::MarketListing& listing) {
        (void)character_id;
        (void)items;
        (void)wallet;
        (void)jobs;
        (void)known_recipes;
        (void)warehouse;
        (void)listing;
        return svc::MarketSaveResult{svc::CallStatus::Unavailable, std::string(), false};
    }
    /// Marketplace sales (migration 0031): a purchase, a removal and an expiry, each ONE call of the character service and ONE transaction there.  All three block and are made on
    /// the world's market worker, which asks again (the same request) until the answer is clear: Ok with an outcome that is stored (Done, Replayed) or a refusal for a reason of
    /// the listing (nothing written), a refusal (Refused, NotFound) or Unsupported.  Unavailable and Failed settle nothing: the step may or may not be stored.  A purchase and a
    /// removal write the character's bags, so they wait for a same-character inventory write in flight and drop a queued older bag, as the listing's save does.  A sink that cannot
    /// ask answers Unavailable.
    virtual svc::BuyResult buy_listing_now(const svc::BuyRequest& request) {
        (void)request;
        svc::BuyResult r;
        r.status = svc::CallStatus::Unavailable;
        return r;
    }
    virtual svc::RemoveResult remove_listing_now(const svc::RemoveRequest& request) {
        (void)request;
        svc::RemoveResult r;
        r.status = svc::CallStatus::Unavailable;
        return r;
    }
    virtual svc::ExpireResult expire_listing_now(const std::string& listing_id, const std::string& item_name) {
        (void)listing_id;
        (void)item_name;
        svc::ExpireResult r;
        r.status = svc::CallStatus::Unavailable;
        return r;
    }
    /// Mail: the claim of attachments AND of the money of mails, in ONE call (see claim_mail_items).  With no money mail it is claim_mail_items.  A sink that cannot ask answers
    /// Unavailable.
    virtual svc::MailClaimResult claim_mail(const std::string& character_id, const std::vector<svc::MailClaim>& claims, const std::vector<svc::MoneyClaim>& money_mails,
                                            std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                            std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse) {
        if (money_mails.empty()) return claim_mail_items(character_id, claims, std::move(items), std::move(wallet), std::move(jobs), std::move(known_recipes), std::move(warehouse));
        svc::MailClaimResult result;
        result.status = svc::CallStatus::Unavailable;
        return result;
    }
    /// Player trading (migration 0032): commits a two-character atomic swap in ONE database transaction.
    /// Waits for any in-flight inventory writes for either participant and drops queued older bags for both.
    /// A sink that cannot ask answers Unavailable.
    virtual svc::CommitTradeResult commit_player_trade_now(const svc::CommitTradeRequest& request) {
        (void)request;
        svc::CommitTradeResult result;
        result.status = svc::CallStatus::Unavailable;
        return result;
    }
    /// Social, step 6: the two lists of a progress hand-over the sink still holds for the character and has not written yet (the latest one), when that hand-over carries them.  A sink that
    /// keeps nothing (a test's) answers false.
    virtual bool pending_ignored_names(const std::string& character_id, std::vector<std::string>& players, std::vector<std::string>& guilds) {
        (void)character_id;
        (void)players;
        (void)guilds;
        return false;
    }
    /// Social, step 7: the friends and the pending requests of a progress hand-over the sink still holds for the character and has not written yet (the latest one), when that
    /// hand-over carries both lists.  A sink that keeps nothing (a test's) answers false.
    virtual bool pending_friends(const std::string& character_id, std::vector<svc::Friend>& friends, std::vector<svc::FriendRequest>& requests) {
        (void)character_id;
        (void)friends;
        (void)requests;
        return false;
    }
    /// Social, step 7: the character with this name (the case of the letters does not count), for the friend request by name and the name check.  The lookup is the
    /// character service's; a sink that cannot ask (a test's) answers Failed.  Called from the network thread when a friend call asks.
    virtual svc::CharacterResult character_by_name(const std::string& name) {
        (void)name;
        return {};
    }
    /// Social, step 7: the whole character by its id, both friend lists included, for the other side of an accept or a removal while it has no session here.
    virtual svc::CharacterResult character_by_id(const std::string& id) {
        (void)id;
        return {};
    }
    /// Social, step 7: queues a character's two friend lists to be saved whole (the other side of a request, an accept or a removal while it has no session here).
    /// Never blocks on the network; latest wins, as save_progress does.
    virtual void save_friend_lists(const std::string& character_id, std::vector<svc::Friend> friends, std::vector<svc::FriendRequest> requests) {
        (void)character_id;
        (void)friends;
        (void)requests;
    }
};

class CharacterSaver : public PositionSink {
public:
    struct Config {
        size_t                    max_queued = 4096;                       // characters waiting at once
        int                       max_attempts = 3;                        // tries for a save the service answers Unavailable
        std::chrono::milliseconds retry_delay{1000};                       // before try n + 1: n times this
        std::chrono::milliseconds destructor_wait{2000};                   // the destructor's own flush
    };

    explicit CharacterSaver(svc::CharacterApi& api, Config config = {});
    ~CharacterSaver() override;
    CharacterSaver(const CharacterSaver&) = delete;
    CharacterSaver& operator=(const CharacterSaver&) = delete;

    void save(const std::string& character_id, const svc::Position& position) override;
    void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items) override;
    void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet) override;
    void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs) override;
    void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                        std::vector<int64_t> known_recipes) override;
    void save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                        std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse) override;
    void save_progress(const std::string& character_id, svc::Progress progress) override;
    /// Social, step 9: a whole-guild save and a character's cooldown, queued behind the character saves (each guild's latest save wins).
    void save_guild(const svc::Guild& guild) override;
    void save_guild_cooldown(const std::string& character_id, int64_t until_ms) override;
    /// Social, step 9: the create of a guild, asked of the character service at once (see PositionSink).
    svc::CallStatus create_guild_now(const svc::Guild& guild) override;
    /// Social, step 6: the ignored players and guilds of the progress the saver still holds for the character (see PositionSink).
    bool pending_ignored_names(const std::string& character_id, std::vector<std::string>& players, std::vector<std::string>& guilds) override;
    /// Social, step 7: the friends and the pending requests of the progress the saver still holds for the character (see PositionSink).
    bool pending_friends(const std::string& character_id, std::vector<svc::Friend>& friends, std::vector<svc::FriendRequest>& requests) override;
    /// Social, step 7: the character service's by-name and by-id lookups (see PositionSink); the call is made where the caller stands.  The by-id read first waits until every progress this
    /// saver still holds for the character is written (one path per character: a change made from what it reads keeps what the change before it wrote).
    svc::CharacterResult character_by_name(const std::string& name) override;
    svc::CharacterResult character_by_id(const std::string& id) override;
    /// Social, step 7: queues the two lists as a progress hand-over that carries them and nothing else.
    void save_friend_lists(const std::string& character_id, std::vector<svc::Friend> friends, std::vector<svc::FriendRequest> requests) override;
    /// Mail: the three blocking forwards to the character service (see PositionSink).
    svc::MailsResult load_mails(const std::string& character_id) override;
    svc::MailSaveResult save_mail(const std::string& character_id, const svc::Mail& mail) override;
    svc::MailResult create_mail(const std::string& character_id, const svc::NewMail& mail) override;
    /// Mail: the claim, one call (see PositionSink); it waits for a same-character inventory write in flight and drops a queued older bag.
    svc::MailClaimResult claim_mail_items(const std::string& character_id, const std::vector<svc::MailClaim>& claims, std::vector<svc::InventoryItem> items,
                                          std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs, std::vector<int64_t> known_recipes,
                                          std::vector<svc::WarehouseItem> warehouse) override;

    /// A write of the character is queued, or the worker is writing it now (including the pause before a retry): see PositionSink.  A saver that gave up its queue is never busy.
    bool character_busy(const std::string& character_id) const override;
    /// Marketplace: the two blocking forwards to the character service (see PositionSink); the save also waits for a same-character inventory write in flight and
    /// drops a queued older bag, as claim_mail_items does.
    svc::MarketListingsResult list_market_listings() override;
    svc::MarketSaveResult save_inventory_and_listing_now(const std::string& character_id, std::vector<svc::InventoryItem> items,
                                                         std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                                         std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse,
                                                         const svc::MarketListing& listing) override;
    /// The saver's own pause before a retry (Config::retry_delay): the world's market worker paces its repeated calls the same way.
    std::chrono::milliseconds retry_delay() const override { return config_.retry_delay; }
    /// Marketplace sales (migration 0031): the blocking forwards (see PositionSink); a purchase and a removal also wait for a same-character inventory write in flight.
    svc::BuyResult buy_listing_now(const svc::BuyRequest& request) override;
    svc::RemoveResult remove_listing_now(const svc::RemoveRequest& request) override;
    svc::ExpireResult expire_listing_now(const std::string& listing_id, const std::string& item_name) override;
    svc::MailClaimResult claim_mail(const std::string& character_id, const std::vector<svc::MailClaim>& claims, const std::vector<svc::MoneyClaim>& money_mails,
                                    std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                    std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse) override;
    svc::CommitTradeResult commit_player_trade_now(const svc::CommitTradeRequest& request) override;

    /// Waits until the queue is empty and no call is running, at most `max_wait`; true when it was.
    bool flush(std::chrono::milliseconds max_wait);

    /// Position calls that returned Ok / calls (of either kind) that were given up on (refused, or out of attempts) / saves dropped for the
    /// bound or replaced by a newer one while they waited (not an error: latest wins) / characters waiting now.
    size_t saved() const;
    size_t failed() const;
    size_t dropped() const;
    size_t replaced() const;
    size_t queued() const;
    /// Plan B2: inventory calls that returned Ok.
    size_t inventories_saved() const;
    /// Plan D1: progress calls that returned Ok.
    size_t progress_saved() const;

private:
    struct Entry {
        std::optional<svc::Position>                   position;
        std::optional<std::vector<svc::InventoryItem>> inventory;
        std::optional<std::vector<svc::WalletEntry>>   wallet;      // the vendors milestone, step 0: saved in the inventory's call when set
        std::optional<std::vector<svc::ProcessingJob>> jobs;        // crafting, step 1: saved in the inventory's call when set
        std::optional<std::vector<int64_t>>            known_recipes;   // crafting, step 3: saved in the inventory's call when set
        std::optional<std::vector<svc::WarehouseItem>> warehouse;       // storage, step 0: saved in the inventory's call when set
        std::optional<svc::Progress>                   progress;   // plan D1: saved between the position and the inventory
        /// Social, step 9: a whole guild (the entry's key starts with kGuildKey) or a character's cooldown (kGuildCooldownKey), saved after the inventory.
        std::optional<svc::Guild>                      guild;
        std::optional<std::pair<std::string, int64_t>> guild_cooldown;   // (the character, the cooldown's Unix milliseconds)
        int                                            position_attempts = 0;
        int                                            inventory_attempts = 0;
        int                                            progress_attempts = 0;
        int                                            guild_attempts = 0;
        int                                            guild_cooldown_attempts = 0;
    };

    void run();
    /// Waits for an inventory write of this character that is running now and drops a queued older one: what the claim does first.  False when the saver is stopping.
    bool claim_the_inventory_write(const std::string& character_id);
    /// Waits until neither `a` nor `b` has an in-flight inventory write and drops any queued older inventory writes for both.
    bool claim_two_inventory_writes(const std::string& a, const std::string& b);
    void save_inventory_entry(const std::string& character_id, std::vector<svc::InventoryItem> items, std::optional<std::vector<svc::WalletEntry>> wallet,
                              std::optional<std::vector<svc::ProcessingJob>> jobs = std::nullopt, std::optional<std::vector<int64_t>> known_recipes = std::nullopt,
                              std::optional<std::vector<svc::WarehouseItem>> warehouse = std::nullopt);

    svc::CharacterApi&   api_;
    const Config         config_;
    mutable std::mutex   mu_;
    std::condition_variable cv_;        // the worker waits here: work or stop
    std::condition_variable idle_cv_;   // flush waits here
    std::deque<std::string>                  order_;     // the characters waiting, oldest first
    std::unordered_map<std::string, Entry>   entries_;   // their latest saves; the same ids as order_
    bool   in_flight_ = false;
    std::string in_flight_character_;      // the entry the worker is writing (its progress is what a by-id read waits for)
    bool   in_flight_progress_ = false;
    bool   in_flight_inventory_ = false;
    bool   stop_ = false;
    bool   abandon_ = false;
    size_t saved_ = 0, inventories_saved_ = 0, progress_saved_ = 0, failed_ = 0, dropped_ = 0, replaced_ = 0;
    std::thread thread_;
};

}}  // namespace aoc::worldd
