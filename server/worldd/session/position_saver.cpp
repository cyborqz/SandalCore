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
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        const auto it = entries_.find(character_id);
        if (it != entries_.end()) {
            if (it->second.inventory) ++replaced_;
            it->second.inventory = std::move(items);
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
            if (it->second.progress) ++replaced_;
            it->second.progress = std::move(progress);
            it->second.progress_attempts = 0;
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
                inventory_status = api_.save_inventory(id, *entry.inventory);
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
        idle_cv_.notify_all();
    }
}

}}  // namespace aoc::worldd
