// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/position_saver.cpp
//
//  See position_saver.h.
// ============================================================================
#include "session/position_saver.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc { namespace worldd {

namespace {

/// True when `progress` carries only the two friend lists and would leave every other part of the character alone (what save_friend_lists builds).
bool friend_lists_only(const svc::Progress& progress) {
    svc::Progress rest = progress;
    rest.replace_friends = false;
    rest.friends.clear();
    rest.replace_friend_requests = false;
    rest.friend_requests.clear();
    return rest == svc::Progress{};
}

}  // namespace

CharacterSaver::CharacterSaver(svc::CharacterApi& api, Config config) : api_(api), config_(config), thread_([this] { run(); }) {}

CharacterSaver::~CharacterSaver() {
    if (!flush(config_.destructor_wait)) {
        std::lock_guard<std::mutex> lock(mu_);
        spdlog::warn("[saver] stopping with {} character(s) not saved", order_.size() + (in_flight_ ? 1 : 0));
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        abandon_ = true;
    }
    cv_.notify_all();
    idle_cv_.notify_all();
    thread_.join();
}

void CharacterSaver::save(const std::string& character_id, const svc::Position& position) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        const auto it = entries_.find(character_id);
        if (it != entries_.end()) {
            if (it->second.position) ++replaced_;   // latest wins, in the older entry's place
            it->second.position = position;
            it->second.position_attempts = 0;
        } else {
            if (entries_.size() >= config_.max_queued) {
                const std::string oldest = std::move(order_.front());
                order_.pop_front();
                entries_.erase(oldest);
                ++dropped_;
                spdlog::error("[saver] {} characters are waiting: the oldest ({}) is dropped", config_.max_queued, oldest);
            }
            order_.push_back(character_id);
            Entry e;
            e.position = position;
            entries_.emplace(character_id, std::move(e));
        }
    }
    cv_.notify_one();
}

void CharacterSaver::save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items) {
    save_inventory_entry(character_id, std::move(items), std::nullopt);
}

void CharacterSaver::save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet) {
    save_inventory_entry(character_id, std::move(items), std::move(wallet));
}

void CharacterSaver::save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs) {
    save_inventory_entry(character_id, std::move(items), std::move(wallet), std::move(jobs));
}

void CharacterSaver::save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                    std::vector<int64_t> known_recipes) {
    save_inventory_entry(character_id, std::move(items), std::move(wallet), std::move(jobs), std::move(known_recipes));
}

void CharacterSaver::save_inventory(const std::string& character_id, std::vector<svc::InventoryItem> items, std::vector<svc::WalletEntry> wallet, std::vector<svc::ProcessingJob> jobs,
                                    std::vector<int64_t> known_recipes, std::vector<svc::WarehouseItem> warehouse) {
    save_inventory_entry(character_id, std::move(items), std::move(wallet), std::move(jobs), std::move(known_recipes), std::move(warehouse));
}

void CharacterSaver::save_inventory_entry(const std::string& character_id, std::vector<svc::InventoryItem> items, std::optional<std::vector<svc::WalletEntry>> wallet,
                                          std::optional<std::vector<svc::ProcessingJob>> jobs, std::optional<std::vector<int64_t>> known_recipes,
                                          std::optional<std::vector<svc::WarehouseItem>> warehouse) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        const auto it = entries_.find(character_id);
        if (it != entries_.end()) {
            if (it->second.inventory) ++replaced_;
            it->second.inventory = std::move(items);
            it->second.wallet = std::move(wallet);
            it->second.jobs = std::move(jobs);
            it->second.known_recipes = std::move(known_recipes);
            it->second.warehouse = std::move(warehouse);
            it->second.inventory_attempts = 0;
        } else {
            if (entries_.size() >= config_.max_queued) {
                const std::string oldest = std::move(order_.front());
                order_.pop_front();
                entries_.erase(oldest);
                ++dropped_;
                spdlog::error("[saver] {} characters are waiting: the oldest ({}) is dropped", config_.max_queued, oldest);
            }
            order_.push_back(character_id);
            Entry e;
            e.inventory = std::move(items);
            e.wallet = std::move(wallet);
            e.jobs = std::move(jobs);
            e.known_recipes = std::move(known_recipes);
            e.warehouse = std::move(warehouse);
            entries_.emplace(character_id, std::move(e));
        }
    }
    cv_.notify_one();
}
// [C O A + Cyb]

void CharacterSaver::save_progress(const std::string& character_id, svc::Progress progress) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        const auto it = entries_.find(character_id);
        if (it != entries_.end()) {
            // A friend-lists save that lands while a full progress waits is merged into it: the two lists are the newer state, the level, the experience and
            // the rest are the queued progress's, so neither save is lost (finding 3).
            if (it->second.progress && friend_lists_only(progress) && !friend_lists_only(*it->second.progress)) {
                if (progress.replace_friends) {
                    it->second.progress->replace_friends = true;
                    it->second.progress->friends = std::move(progress.friends);
                }
                if (progress.replace_friend_requests) {
                    it->second.progress->replace_friend_requests = true;
                    it->second.progress->friend_requests = std::move(progress.friend_requests);
                }
                it->second.progress_attempts = 0;
                ++replaced_;
            } else {
                if (it->second.progress) ++replaced_;
                it->second.progress = std::move(progress);
                it->second.progress_attempts = 0;
            }
        } else {
            if (entries_.size() >= config_.max_queued) {
                const std::string oldest = std::move(order_.front());
                order_.pop_front();
                entries_.erase(oldest);
                ++dropped_;
                spdlog::error("[saver] {} characters are waiting: the oldest ({}) is dropped", config_.max_queued, oldest);
            }
            order_.push_back(character_id);
            Entry e;
            e.progress = std::move(progress);
            entries_.emplace(character_id, std::move(e));
        }
    }
    cv_.notify_one();
}

bool CharacterSaver::pending_ignored_names(const std::string& character_id, std::vector<std::string>& players, std::vector<std::string>& guilds) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = entries_.find(character_id);
    if (it == entries_.end() || !it->second.progress || !it->second.progress->replace_ignored_names) return false;
    players = it->second.progress->ignored_character_names;
    guilds = it->second.progress->ignored_guild_names;
    return true;
}

bool CharacterSaver::pending_friends(const std::string& character_id, std::vector<svc::Friend>& friends, std::vector<svc::FriendRequest>& requests) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = entries_.find(character_id);
    if (it == entries_.end() || !it->second.progress || !it->second.progress->replace_friends || !it->second.progress->replace_friend_requests) return false;
    friends = it->second.progress->friends;
    requests = it->second.progress->friend_requests;
    return true;
}

svc::CharacterResult CharacterSaver::character_by_name(const std::string& name) { return api_.get_character_by_name(name); }

svc::CharacterResult CharacterSaver::character_by_id(const std::string& id) {
    {
        // One path per character: the read waits until the progress this saver still holds for the character (queued, or in the worker now) is written, so a
        // change made from what it reads keeps what the change before it wrote (review finding 1).
        std::unique_lock<std::mutex> lock(mu_);
        idle_cv_.wait(lock, [this, &id] {
            if (in_flight_ && in_flight_progress_ && in_flight_character_ == id) return false;
            const auto it = entries_.find(id);
            return it == entries_.end() || !it->second.progress;
        });
    }
    return api_.get_character(id);
}

void CharacterSaver::save_friend_lists(const std::string& character_id, std::vector<svc::Friend> friends, std::vector<svc::FriendRequest> requests) {
    svc::Progress p;
    p.replace_friends = true;
    p.friends = std::move(friends);
    p.replace_friend_requests = true;
    p.friend_requests = std::move(requests);
    save_progress(character_id, std::move(p));
}

bool CharacterSaver::flush(std::chrono::milliseconds max_wait) {
    std::unique_lock<std::mutex> lock(mu_);
    return idle_cv_.wait_for(lock, max_wait, [this] { return (order_.empty() && !in_flight_) || abandon_; }) && order_.empty() && !in_flight_;
}

size_t CharacterSaver::saved() const { std::lock_guard<std::mutex> l(mu_); return saved_; }
size_t CharacterSaver::inventories_saved() const { std::lock_guard<std::mutex> l(mu_); return inventories_saved_; }
size_t CharacterSaver::progress_saved() const { std::lock_guard<std::mutex> l(mu_); return progress_saved_; }
size_t CharacterSaver::failed() const { std::lock_guard<std::mutex> l(mu_); return failed_; }
size_t CharacterSaver::dropped() const { std::lock_guard<std::mutex> l(mu_); return dropped_; }
size_t CharacterSaver::replaced() const { std::lock_guard<std::mutex> l(mu_); return replaced_; }
size_t CharacterSaver::queued() const { std::lock_guard<std::mutex> l(mu_); return order_.size(); }

void CharacterSaver::run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || !order_.empty(); });
        if (abandon_ || order_.empty()) return;   // stopping: with nothing left, or left behind on purpose
        const std::string id = std::move(order_.front());
        order_.pop_front();
        const auto it = entries_.find(id);
        Entry entry = std::move(it->second);
        entries_.erase(it);
        in_flight_ = true;
        in_flight_character_ = id;
        in_flight_progress_ = entry.progress.has_value();
        lock.unlock();
        svc::CallStatus position_status = svc::CallStatus::Ok, inventory_status = svc::CallStatus::Ok, progress_status = svc::CallStatus::Ok;
        if (entry.position) {
            position_status = svc::CallStatus::Failed;
            try {
                position_status = api_.save_position(id, *entry.position);
            } catch (const std::exception& e) {
                spdlog::error("[saver] saving the position of {} threw {}", id, e.what());
            }
        }
        if (entry.progress) {
            progress_status = svc::CallStatus::Failed;
            try {
                progress_status = api_.save_progress(id, *entry.progress);
            } catch (const std::exception& e) {
                spdlog::error("[saver] saving the progress of {} threw {}", id, e.what());
            }
        }
        if (entry.inventory) {
            inventory_status = svc::CallStatus::Failed;
            try {
                inventory_status = entry.wallet && entry.jobs && entry.known_recipes && entry.warehouse ? api_.save_inventory(id, *entry.inventory, *entry.wallet, *entry.jobs, *entry.known_recipes, *entry.warehouse)
                                   : entry.wallet && entry.jobs && entry.known_recipes ? api_.save_inventory(id, *entry.inventory, *entry.wallet, *entry.jobs, *entry.known_recipes)
                                   : entry.wallet && entry.jobs   ? api_.save_inventory(id, *entry.inventory, *entry.wallet, *entry.jobs)
                                   : entry.wallet              ? api_.save_inventory(id, *entry.inventory, *entry.wallet)
                                                               : api_.save_inventory(id, *entry.inventory);
            } catch (const std::exception& e) {
                spdlog::error("[saver] saving the inventory of {} threw {}", id, e.what());
            }
        }
        lock.lock();
        // in_flight_ stays set through the pause before a retry: flush() must not report an empty queue while one is due
        bool retry_position = false, retry_inventory = false, retry_progress = false;
        if (entry.position) {
            switch (position_status) {
                case svc::CallStatus::Ok:
                    ++saved_;
                    spdlog::debug("[saver] {}: position saved ({:.1f}, {:.1f}, {:.1f}) cm, yaw {:.1f}", id, entry.position->x, entry.position->y,
                                  entry.position->z, entry.position->yaw);
                    break;
                case svc::CallStatus::Unavailable:
                    if (++entry.position_attempts < config_.max_attempts && !stop_) {
                        spdlog::warn("[saver] {}: the character service is unavailable: position try {} of {}", id, entry.position_attempts + 1,
                                     config_.max_attempts);
                        retry_position = true;
                        break;
                    }
                    [[fallthrough]];
                default:
                    ++failed_;
                    spdlog::error("[saver] {}: the position could not be saved ({}): given up", id, svc::call_status_name(position_status));
                    break;
            }
        }
        if (entry.progress) {
            switch (progress_status) {
                case svc::CallStatus::Ok:
                    ++progress_saved_;
                    spdlog::debug("[saver] {}: progress saved (level {}, class {}, {} XP row(s))", id, entry.progress->level, entry.progress->class_id,
                                  entry.progress->experience.size());
                    break;
                case svc::CallStatus::Unavailable:
                    if (++entry.progress_attempts < config_.max_attempts && !stop_) {
                        spdlog::warn("[saver] {}: the character service is unavailable: progress try {} of {}", id, entry.progress_attempts + 1,
                                     config_.max_attempts);
                        retry_progress = true;
                        break;
                    }
                    [[fallthrough]];
                default:
                    ++failed_;
                    spdlog::error("[saver] {}: the progress (level {}, class {}) could not be saved ({}): given up", id, entry.progress->level,
                                  entry.progress->class_id, svc::call_status_name(progress_status));
                    break;
            }
        }
        if (entry.inventory) {
            switch (inventory_status) {
                case svc::CallStatus::Ok:
                    ++inventories_saved_;
                    spdlog::debug("[saver] {}: inventory saved ({} items)", id, entry.inventory->size());
                    break;
                case svc::CallStatus::Unavailable:
                    if (++entry.inventory_attempts < config_.max_attempts && !stop_) {
                        spdlog::warn("[saver] {}: the character service is unavailable: inventory try {} of {}", id, entry.inventory_attempts + 1,
                                     config_.max_attempts);
                        retry_inventory = true;
                        break;
                    }
                    [[fallthrough]];
                default:
                    ++failed_;
                    spdlog::error("[saver] {}: the inventory ({} items) could not be saved ({}): given up", id, entry.inventory->size(),
                                  svc::call_status_name(inventory_status));
                    break;
            }
        }
        if (retry_position || retry_inventory || retry_progress) {
            // try again after a pause, unless a newer save came in meanwhile (that one is tried instead): each part on its own
            const int attempts = std::max({retry_position ? entry.position_attempts : 0, retry_inventory ? entry.inventory_attempts : 0,
                                           retry_progress ? entry.progress_attempts : 0});
            cv_.wait_for(lock, config_.retry_delay * attempts, [this] { return abandon_; });
            if (!abandon_) {
                Entry again;
                if (retry_position) {
                    again.position = entry.position;
                    again.position_attempts = entry.position_attempts;
                }
                if (retry_inventory) {
                    again.inventory = std::move(entry.inventory);
                    again.wallet = std::move(entry.wallet);
                    again.jobs = std::move(entry.jobs);                  // (a retried save keeps everything its call carried)
                    again.known_recipes = std::move(entry.known_recipes);
                    again.warehouse = std::move(entry.warehouse);
                    again.inventory_attempts = entry.inventory_attempts;
                }
                if (retry_progress) {
                    again.progress = std::move(entry.progress);
                    again.progress_attempts = entry.progress_attempts;
                }
                const auto newer = entries_.find(id);
                if (newer == entries_.end()) {
                    order_.push_front(id);
                    entries_.emplace(id, std::move(again));
                } else {
                    if (!newer->second.position && again.position) {
                        newer->second.position = again.position;
                        newer->second.position_attempts = again.position_attempts;
                    }
                    if (!newer->second.inventory && again.inventory) {
                        newer->second.inventory = std::move(again.inventory);
                        newer->second.wallet = std::move(again.wallet);
                        newer->second.jobs = std::move(again.jobs);
                        newer->second.known_recipes = std::move(again.known_recipes);
                        newer->second.warehouse = std::move(again.warehouse);
                        newer->second.inventory_attempts = again.inventory_attempts;
                    }
                    if (!newer->second.progress && again.progress) {
                        newer->second.progress = std::move(again.progress);
                        newer->second.progress_attempts = again.progress_attempts;
                    }
                }
            }
        }
        in_flight_ = false;
        in_flight_character_.clear();
        in_flight_progress_ = false;
        idle_cv_.notify_all();
    }
}

}}  // namespace aoc::worldd
