// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/endpoint_phases.cpp: the sessions part of the tick with the players' view passes on several threads (EndpointConfig::view_threads; session/view_pool.h)
//
//  The loop of Endpoint::tick runs one session after the other: its own work, its view's pass, its flush.  A pass reads the world's registry and writes nothing but its own view and
//  connection, so with a pool the same work is run in three phases:
//    1  every session's own work in turn (PlayerSession::tick_logic: the abilities, the mounts, the quests ...; it also decides, in turn, whether the view's pass is due, because the views
//       share a count of phases),
//    2  all the due passes at once on the pool, with the registry frozen (a change of it throws), each on its own view and connection,
//    3  every session's rest in turn (what follows the view, the answer to a failed session, the flush, the acknowledgements).
//  A session's own work, its pass and its flush still happen in that order for each session, and the sessions are walked in the same order in phases 1 and 3.  What differs from the loop is
//  that a pass sees the changes that sessions after its own made in the same tick (the loop ran their work after its pass): the order of things in one tick, not what is sent in the long run.
//  With 1 thread the passes run in order on this thread (the reference the other counts are compared with).
// ============================================================================
#include "net/endpoint.h"
#include "net/tick_profile.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>

namespace aoc { namespace worldd {

namespace {

using SteadyClock = std::chrono::steady_clock;

double micros(SteadyClock::time_point from, SteadyClock::time_point to) { return std::chrono::duration<double, std::micro>(to - from).count(); }

/// Freezes the registry for the passes and thaws it again however they end.
class Frozen {
public:
    explicit Frozen(world::ActorRegistry& r) : r_(r) { r_.freeze(true); }
    ~Frozen() { r_.freeze(false); }
    Frozen(const Frozen&) = delete;
    Frozen& operator=(const Frozen&) = delete;

private:
    world::ActorRegistry& r_;
};

}  // namespace

void Endpoint::tick_in_phases(Clock::time_point now, std::vector<std::pair<Address, const char*>>& drop) {
    TickProfile& profile = tick_profile();
    const bool timing = profile.on;
    struct Item {
        const Address*          addr = nullptr;
        Session*                s = nullptr;
        PlayerSession::TickPlan plan;
        bool                    dropped = false;   // dropped in phase 1: nothing more is done for it
        double                  job_us = 0;        // its pass, on whichever thread ran it
        bool                    failed = false;    // its pass threw (whatever the exception said)
        std::string             error;             // what its pass threw
    };
    std::vector<Item> items;
    items.reserve(sessions_.size());

    // phase 1: every session's own work, in turn
    for (auto& [addr, s] : sessions_) {
        if (s->connection.closed_by_client()) {
            drop.emplace_back(addr, "the client quit");
            continue;
        }
        if (s->connection.timed_out(now)) {
            drop.emplace_back(addr, "timed out");
            continue;
        }
        if (s->connection.failed()) {
            drop.emplace_back(addr, "broke the protocol");
            continue;
        }
        if (!s->closing && s->control.state() < ControlChannel::State::AwaitJoin && now >= s->deadline) {
            drop.emplace_back(addr, "did not log in in time");
            continue;
        }
        Item item;
        item.addr = &addr;
        item.s = s.get();
        try {
            SteadyClock::time_point a{};
            if (timing) a = SteadyClock::now();
            if (s->player && !s->closing) item.plan = s->player->tick_logic(s->connection, now, /*plan_view=*/true);
            if (timing) {
                const double us = micros(a, SteadyClock::now());
                profile.sessions_us += us;
                s->profile_us += us;
            }
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): the tick threw {}: dropping the session", addr.to_string(), s->request.character_id, e.what());
            drop.emplace_back(addr, "threw an exception");
            item.dropped = true;
        }
        items.push_back(std::move(item));
    }

    // phase 2: all the due passes at once
    std::vector<Item*> jobs;
    for (Item& item : items) {
        if (!item.dropped && item.s->player && !item.s->closing && item.plan.view && !item.plan.stopped) jobs.push_back(&item);
    }
    if (!jobs.empty()) {
        std::vector<ViewPool::JobError> errors(jobs.size());
        SteadyClock::time_point a{};
        if (timing) a = SteadyClock::now();
        {
            Frozen frozen(cfg_.players->registry());
            view_pool_->run(
                jobs.size(),
                [&](size_t k) {
                    Item& item = *jobs[k];
                    const auto started = SteadyClock::now();
                    item.s->player->run_view(item.s->connection, item.plan, now);
                    item.job_us = micros(started, SteadyClock::now());
                },
                &errors);
        }
        if (timing) profile.sessions_us += micros(a, SteadyClock::now());   // (the passes' wall time; the view's own figures are the threads' added up)
        for (size_t k = 0; k < jobs.size(); ++k) {
            jobs[k]->failed = errors[k].failed;
            jobs[k]->error = std::move(errors[k].what);
            if (timing) {
                jobs[k]->s->profile_us += jobs[k]->job_us;
                jobs[k]->s->profile_view_us += jobs[k]->job_us;
            }
        }
    }

    // phase 3: every session's rest, in turn
    for (Item& item : items) {
        if (item.dropped) continue;
        Session* s = item.s;
        const Address& addr = *item.addr;
        try {
            if (item.failed) throw std::runtime_error(item.error.empty() ? "the view pass threw an exception without a message" : item.error);   // (a pass that threw: the session is dropped, as when a tick throws)
            SteadyClock::time_point b{}, c{};
            if (timing) b = SteadyClock::now();
            if (s->player && !s->closing) {
                if (!item.plan.stopped) s->player->tick_after(now, item.plan);
                if (s->player->failed()) apply(*s, s->control.kick(login_text::kLoginFailed), now);
            }
            if (timing) c = SteadyClock::now();
            for (auto& pkt : s->connection.flush(now)) send_(addr, pkt);
            if (timing) {
                const auto d = SteadyClock::now();
                profile.sessions_us += micros(b, c);
                profile.flush_us += micros(c, d);
                s->profile_us += micros(b, d);
            }
            if (s->player) s->player->take_acks(s->connection);   // what flush dropped counts as lost
            if (s->closing && (now >= s->close_by || s->connection.unacked_reliable(0) == 0)) drop.emplace_back(addr, "closed");
        } catch (const std::exception& e) {
            spdlog::error("[endpoint] {} ({}): the tick threw {}: dropping the session", addr.to_string(), s->request.character_id, e.what());
            drop.emplace_back(addr, "threw an exception");
        }
    }
}

}}  // namespace aoc::worldd
