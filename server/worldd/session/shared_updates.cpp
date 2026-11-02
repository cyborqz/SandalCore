// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/shared_updates.cpp
//
//  See shared_updates.h.
// ============================================================================
#include "session/shared_updates.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace aoc::worldd {

ShareMode SharedUpdates::parse(std::string_view text, bool* ok) {
    if (ok) *ok = true;
    if (text == "off") return ShareMode::Off;
    if (text == "on") return ShareMode::On;
    if (text == "verify") return ShareMode::Verify;
    if (ok) *ok = false;
    return ShareMode::Off;
}

const char* SharedUpdates::name(ShareMode mode) {
    switch (mode) {
        case ShareMode::Off: return "off";
        case ShareMode::On: return "on";
        case ShareMode::Verify: return "verify";
    }
    return "off";
}

namespace {

/// What the test programs share (from_test_environment).  When the program ends it says so if a shared update differed from the plain build: the runner counts a `[FAIL]` line although
/// the program exits 0, and the checks of the suites are not about sharing, so a difference would otherwise go unseen.
struct TestSharing {
    std::shared_ptr<SharedUpdates> shared;
    ~TestSharing() {
        if (shared && shared->stats().mismatches != 0) {
            std::printf("  [FAIL] shared updates: %llu shared update(s) differed from the update built the plain way (the log's [share-updates] line says which mob)\n",
                        static_cast<unsigned long long>(shared->stats().mismatches));
        }
    }
};

}  // namespace

std::shared_ptr<SharedUpdates> SharedUpdates::from_test_environment() {
    static const TestSharing instance{[]() -> std::shared_ptr<SharedUpdates> {
        // the programs run the way worldd does (sharing on), with every shared update also built the plain way and compared (they are small); the variable picks another mode
        ShareMode mode = ShareMode::Verify;
        const char* text = std::getenv("AOC_TEST_SHARE_UPDATES");
        bool ok = true;
        if (text && *text) {
            const ShareMode asked = parse(text, &ok);
            if (ok) mode = asked;
        }
        return mode != ShareMode::Off ? std::make_shared<SharedUpdates>(mode, 1) : nullptr;
    }()};
    return instance.shared;
}

const UpdateMemo::Variant* SharedUpdates::find(const world::WorldActor& actor, uint32_t from_version) const {
    const UpdateMemo* memo = actor.update_memo.ptr.get();
    if (!memo || memo->version != actor.version || memo->shape_epoch != actor.shape_epoch) return nullptr;
    for (const UpdateMemo::Variant& v : memo->variants) {
        if (v.from_version == from_version) return &v;
    }
    return nullptr;
}

const UpdateMemo::Variant& SharedUpdates::publish(world::WorldActor& actor, uint32_t from_version, std::vector<repl::ContentBlock>&& blocks, repl::SentRecord&& record,
                                                  repl::ShadowRedo&& redo) {
    if (!actor.update_memo.ptr) {
        actor.update_memo.ptr = std::make_shared<UpdateMemo>();
        actor.update_memo.ptr->variants.reserve(UpdateMemo::kMaxVariants);
    }
    UpdateMemo& memo = *actor.update_memo.ptr;
    if (memo.version != actor.version || memo.shape_epoch != actor.shape_epoch) {   // the actor changed: what was built is for a state it has left
        memo.variants.clear();
        memo.version = actor.version;
        memo.shape_epoch = actor.shape_epoch;
    }
    if (memo.variants.size() >= UpdateMemo::kMaxVariants) memo.variants.erase(memo.variants.begin());
    UpdateMemo::Variant& v = memo.variants.emplace_back();
    v.from_version = from_version;
    v.blocks = std::move(blocks);
    v.record = std::move(record);
    v.redo = std::move(redo);
    if (fault_ == Fault::DropLastBlock && !v.blocks.empty()) {   // (a test damages the next update that has something to damage; the verify mode must find out)
        v.blocks.pop_back();
        fault_ = Fault::None;
    } else if (fault_ == Fault::SkewRedo) {
        for (auto& o : v.redo.objects) {
            if (o.rep.empty()) continue;
            o.rep.erase(o.rep.begin());
            fault_ = Fault::None;
            break;
        }
    }
    ++stats_.builds;
    return v;
}

void SharedUpdates::mismatch(uint64_t actor, const char* what) {
    ++stats_.mismatches;
    if (mode_ == ShareMode::Off) return;
    spdlog::error("[share-updates] actor {}: {}; sharing is turned off for the rest of the run", actor, what);
    mode_ = ShareMode::Off;
}

}  // namespace aoc::worldd
