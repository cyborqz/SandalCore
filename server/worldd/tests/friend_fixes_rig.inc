// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// ============================================================================
//  tests/friend_fixes_rig.inc
//
//  The fakes the friends-list review tests share: a character service that keeps its records and can hold one save in the saver's worker, and a
//  lookup of the last progress a sink wrote for a character.
//  (included by the friends-list fix tests below; the include guard keeps it to one definition)
// ============================================================================
#pragma once

#include "session/position_saver.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

/// ASCII letters folded to lower case, for the fake service's by-name answer.
static std::string friend_store_fold(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// A character service that keeps its records and can hold one save in the saver's worker: the two changes to one offline character and the
/// lists-only save's merge are tested against it.
struct FriendStoreApi : svc::CharacterApi {
    /// Holds the worker in the save named by the api's gate_id until release().
    struct Gate {
        std::mutex              mu;
        std::condition_variable cv;
        bool                    entered = false, open = false;

        bool wait_entered() {
            std::unique_lock<std::mutex> lock(mu);
            return cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; });
        }
        void block() {
            std::unique_lock<std::mutex> lock(mu);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return open; });
        }
        void release() {
            std::lock_guard<std::mutex> lock(mu);
            open = true;
            cv.notify_all();
        }
    };

    std::mutex                            mu;
    std::map<std::string, svc::Character> characters;   // id -> the record
    Gate*                                 gate = nullptr;
    std::string                           gate_id;
    bool                                  gate_progress = false;   // true: save_progress is held; false: save_position
    int                                   progress_calls = 0;
    std::vector<svc::Progress>            progress;   // every progress written, in order

    svc::CharacterResult get_character(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = characters.find(id);
        svc::CharacterResult r;
        if (it == characters.end()) {
            r.status = svc::CallStatus::NotFound;
            return r;
        }
        r.status = svc::CallStatus::Ok;
        r.character = it->second;
        return r;
    }

    svc::CharacterResult get_character_by_name(const std::string& name) override {
        std::lock_guard<std::mutex> lock(mu);
        svc::CharacterResult r;
        for (const auto& [id, c] : characters) {
            if (friend_store_fold(c.name) != friend_store_fold(name)) continue;
            r.status = svc::CallStatus::Ok;
            r.character = c;
            return r;
        }
        r.status = svc::CallStatus::NotFound;
        return r;
    }

    svc::CallStatus save_position(const std::string& id, const svc::Position&) override {
        if (gate && !gate_progress && id == gate_id) gate->block();
        return svc::CallStatus::Ok;
    }

    svc::CallStatus save_progress(const std::string& id, const svc::Progress& p) override {
        if (gate && gate_progress && id == gate_id) gate->block();
        std::lock_guard<std::mutex> lock(mu);
        const auto it = characters.find(id);
        if (it == characters.end()) return svc::CallStatus::NotFound;
        svc::Character& c = it->second;
        if (p.level != 0) c.level = p.level;
        if (p.class_id != 0) c.class_id = p.class_id;
        if (!p.experience.empty()) c.experience = p.experience;
        if (p.replace_ignored_names) {
            c.ignored_character_names = p.ignored_character_names;
            c.ignored_guild_names = p.ignored_guild_names;
        }
        if (p.replace_friends) c.friends = p.friends;
        if (p.replace_friend_requests) c.friend_requests = p.friend_requests;
        ++progress_calls;
        progress.push_back(p);
        return svc::CallStatus::Ok;
    }
};

/// The last progress a FriendService wrote for `id`, or nullptr.
template <typename Sink>
static const svc::Progress* friend_progress_of(const Sink& sink, const std::string& id) {
    for (auto it = sink.progress.rbegin(); it != sink.progress.rend(); ++it) {
        if (it->first == id) return &it->second;
    }
    return nullptr;
}

}  // namespace
