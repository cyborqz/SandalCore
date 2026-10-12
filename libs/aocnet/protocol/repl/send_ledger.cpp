// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/send_ledger.cpp
//
//  See send_ledger.h.
// ============================================================================
#include "protocol/repl/send_ledger.h"

#include <algorithm>

namespace aoc { namespace protocol { namespace repl {

void SendLedger::sent(uint64_t chain, bool reliable, size_t parts, const SentRecord& record, ActorShadow& shadow) {
    std::set<Key> keys;
    for (const SentObject& o : record.objects) {
        const uint64_t obj = o.is_actor ? 0 : o.id;
        for (const uint16_t h : o.handles) keys.insert(Key{obj, 0, h, 0});
        for (const SentObject::Fast& f : o.fast) {
            for (const int32_t id : f.changed) keys.insert(Key{obj, 1, f.field, id});
            for (const int32_t id : f.removed) keys.insert(Key{obj, 1, f.field, id});
        }
        if (o.introduced) keys.insert(Key{obj, 2, 0, 0});
    }
    if (keys.empty()) return;   // nothing a NAK would put back
    // the newest chain to carry an entry owns it: an older record gives it up
    for (const Key& k : keys) {
        const auto n = newest_.find(k);
        if (n != newest_.end() && n->second != chain) {
            const auto old = records_.find(n->second);
            if (old != records_.end()) old->second.keys.erase(k);
        }
        newest_[k] = chain;
    }
    Rec r;
    r.reliable = reliable;
    r.parts_left = std::max<size_t>(parts, 1);
    r.keys = std::move(keys);
    records_[chain] = std::move(r);
    if (reliable) ++reliable_pending_;
    // a record that lost every entry to newer chains has nothing to put back: it ends with its events, which find nothing
    while (records_.size() > kMaxRecords) {
        const auto oldest = records_.begin();
        if (!oldest->second.reliable) apply_nak(oldest->second, shadow);
        end(oldest);
    }
}

void SendLedger::end(std::map<uint64_t, Rec>::iterator it) {
    const uint64_t chain = it->first;
    if (it->second.reliable && reliable_pending_ > 0) --reliable_pending_;
    for (const Key& k : it->second.keys) {
        const auto n = newest_.find(k);
        if (n != newest_.end() && n->second == chain) newest_.erase(n);
    }
    records_.erase(it);
}

bool SendLedger::on_event(uint64_t chain, bool delivered, ActorShadow& shadow) {
    const auto it = records_.find(chain);
    if (it == records_.end()) return false;
    Rec& r = it->second;
    if (!delivered) {
        if (r.reliable) return true;   // the Connection sends the part again; its event follows
        apply_nak(r, shadow);
        end(it);
        return true;
    }
    if (r.parts_left > 0 && --r.parts_left == 0) end(it);
    return true;
}

void SendLedger::clear() {
    records_.clear();
    newest_.clear();
    reliable_pending_ = 0;
}
// ~ C-O-A :: Cyb ~

void SendLedger::apply_nak(const Rec& r, ActorShadow& shadow) {
    if (!r.keys.empty()) ++naks_;
    // a subobject's first block first: it resets the subobject's whole shadow, whatever else the chain carried for it
    for (const Key& k : r.keys) {
        if (k.kind != 2) continue;
        const auto it = shadow.subobjects.find(k.obj);
        if (it == shadow.subobjects.end()) continue;
        if (it->second.active.all_active()) shadow.subobjects.erase(it);
        else it->second.forget_sent();
        ++resent_;
    }
    for (const Key& k : r.keys) {
        if (k.kind == 2) continue;
        ObjectShadow* sh = nullptr;
        if (k.obj == 0) {
            sh = &shadow.actor;
        } else {
            const auto it = shadow.subobjects.find(k.obj);
            if (it != shadow.subobjects.end()) sh = &it->second;
        }
        if (!sh) continue;   // the subobject is new again: it is sent whole anyway
        if (k.kind == 0) {
            if (sh->rep.sent.erase(static_cast<uint16_t>(k.a)) != 0) ++resent_;
            continue;
        }
        FastArrayShadow& fa = sh->fast.by_field[k.a];
        bool found = false;
        for (auto& [id, key] : fa.items) {
            if (id != k.b) continue;
            key = kUnknownKey;
            found = true;
            break;
        }
        if (!found) fa.items.emplace_back(k.b, kUnknownKey);
        fa.resend = true;
        ++resent_;
    }
}

}}}  // namespace aoc::protocol::repl
