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
    /// Plan D1: queues the character's progress (SaveProgress: level, class, XP rows). Never blocks on the network.
    virtual void save_progress(const std::string& character_id, svc::Progress progress) {
        (void)character_id;
        (void)progress;
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
    void save_progress(const std::string& character_id, svc::Progress progress) override;

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
        std::optional<svc::Progress>                   progress;   // plan D1: saved between the position and the inventory
        int                                            position_attempts = 0;
        int                                            inventory_attempts = 0;
        int                                            progress_attempts = 0;
    };

    void run();

    svc::CharacterApi&   api_;
    const Config         config_;
    mutable std::mutex   mu_;
    std::condition_variable cv_;        // the worker waits here: work or stop
    std::condition_variable idle_cv_;   // flush waits here
    std::deque<std::string>                  order_;     // the characters waiting, oldest first
    std::unordered_map<std::string, Entry>   entries_;   // their latest saves; the same ids as order_
    bool   in_flight_ = false;
    bool   stop_ = false;
    bool   abandon_ = false;
    size_t saved_ = 0, inventories_saved_ = 0, progress_saved_ = 0, failed_ = 0, dropped_ = 0, replaced_ = 0;
    std::thread thread_;
};

}}  // namespace aoc::worldd
