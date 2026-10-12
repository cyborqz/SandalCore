// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/send_ledger.h
//
//  What a connection owes an actor after a lost packet: UE's NAK handling for replicated properties.
//
//  UE resends the CURRENT value of every property a lost packet carried.  FRepLayout keeps, per sent packet, the changelist it carried
//  (FRepChangedHistory) and ReceivedNak marks those properties dirty again, so the next update sends them from the object's current state
//  (a value that changed again meanwhile goes out as the newest, not the old one); a fast array puts the state from before the lost
//  payload back (FObjectReplicator::ReceivedNak, Retirement.DynamicState).  What the client acknowledged is never sent again.
//
//  The shadow (replicator.h ActorShadow) is "what was last SENT", not "what the client has", so without this a lost update leaves the client
//  with an old value until something changes the property again (for a Stamina item: until the next regeneration tick, and for a value that
//  never changes again, until a relog).  Chains on a RELIABLE chain need none of it: the Connection sends a lost reliable bunch again, with
//  the same bits (worldd's pawn, PlayerController and PlayerState updates are all reliable, and so are an NPC's non-movement changes).  The
//  unreliable ones do: ReplicatedMovement every 67 ms (stage 2's walking NPCs) is what the ledger is for, and any update the caller chooses
//  to send unreliably.
//
//  The ledger is per object channel (one ActorView entry; the pawn's would be one too).  The caller
//    - gets the update's SentRecord from update_actor, sends the chain, and once the Connection accepted it calls sent(chain, ...);
//    - feeds every AckEvent of that chain to on_event(chain, delivered, shadow) (Connection::take_ack_events: one event per part);
//    - calls clear() when the channel closes or opens anew: the client has nothing then, the open sends it all.
//
//  What a NAK does, per entry of the lost chain's record that no newer chain has carried since:
//    a RepLayout handle        the shadow forgets it: the next update writes the handle's current value in full
//    a fast-array item         the shadow's key for the item becomes kUnknownKey (an item the array no longer has is a deletion again, one it
//                              has is a changed item with its current values) and the array is marked for resend even though its key is
//                              the shadow's
//    a subobject's first block the subobject is new again: its whole block and its export go out
//  An entry a newer chain carried (its record was handed to the newer chain: "the newest wins") is left alone: that chain's own fate decides, so
//  a value that was acknowledged in a later packet is not sent again for an earlier loss, and a NAK after the value changed again sends the
//  newest value once.  A chain is a NAK when any of its parts was lost (an unreliable chain with a lost part is useless); a reliable chain
//  ignores lost parts (the Connection sends them again) and ends when every part has been delivered.
//
//  One hazard of mixing reliable and unreliable chains on a channel: a lost reliable bunch is sent again LATER, and the client applies it
//  (and every reliable bunch queued behind it) after the unreliable chains that arrived meanwhile, so what it carries for a handle a newer
//  unreliable chain also carried is the OLD value, which then stands (the server's shadow believes the newer one arrived).  The contract that
//  rules it out is the caller's: send a chain unreliably only while reliable_pending() is false, and reliably otherwise (it queues behind the
//  pending ones, in order).  A reliable chain ends once every part was delivered; one queued behind an older reliable chain ends after it
//  too, because the older one stays pending until its resend is acknowledged and the client applies the queue in order.
//
//  Bounded: at most kMaxRecords chains are tracked; one more evicts the oldest (as a NAK when it is unreliable: the cautious reading of an event
//  that never came), so a connection that stops answering costs a fixed amount, and clear() drops everything.
// ============================================================================
#pragma once

#include "protocol/repl/replicator.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>

namespace aoc { namespace protocol { namespace repl {
// (C | O | A ~ Cyb)

class SendLedger {
public:
    /// Chains tracked at once.  The packet window is 256 and every chain rides in one or a few packets, so this is above what can be in
    /// flight; the oldest beyond it are given up on.
    static constexpr size_t kMaxRecords = 512;

    /// A chain the connection accepted: `record` is what it carried (update_actor's), `parts` the AckEvents it will get (one per part) and
    /// `reliable` whether the Connection resends it itself.  `shadow` is the shadow of the actor as the chain left it (an eviction is a NAK).
    /// An entry that an older chain carried moves to this one.
    void sent(uint64_t chain, bool reliable, size_t parts, const SentRecord& record, ActorShadow& shadow);

    /// One AckEvent of `chain`.  A lost part of an unreliable chain puts what the chain carried back into `shadow` (see above), and ends the
    /// chain; a delivered part counts toward the chain's end.  False when the chain is not tracked (ended, superseded whole, cleared).
    bool on_event(uint64_t chain, bool delivered, ActorShadow& shadow);

    /// The channel closed or opened anew: the client has nothing of what was tracked.
    void clear();

    bool   tracks(uint64_t chain) const { return records_.count(chain) != 0; }
    /// A reliable chain is in flight (not every part delivered).  The caller sends nothing unreliably meanwhile (see above).
    bool   reliable_pending() const { return reliable_pending_ > 0; }
    size_t records() const { return records_.size(); }
    /// Entries (handles, items, first blocks) tracked, over all chains: what a NAK would put back.
    size_t entries() const { return newest_.size(); }
    /// Entries put back by NAKs so far, and the NAKs that had any to put back (a lost chain whose every entry a newer chain took is none).
    size_t resent() const { return resent_; }
    size_t naks() const { return naks_; }

private:
    struct Key {
        uint64_t obj = 0;      // a subobject's ObjectId, 0 for the actor
        uint32_t kind = 0;     // 0 handle, 1 fast-array item, 2 introduced subobject
        uint32_t a = 0;        // the handle, or the fast array's field
        int32_t  b = 0;        // the item's ReplicationID
        auto operator<=>(const Key&) const = default;
    };
    struct Rec {
        bool           reliable = false;
        size_t         parts_left = 0;
        std::set<Key>  keys;
    };

    void apply_nak(const Rec& r, ActorShadow& shadow);
    void end(std::map<uint64_t, Rec>::iterator it);

    std::map<uint64_t, Rec> records_;       // by chain id, oldest first (ChainIds only rise)
    std::map<Key, uint64_t> newest_;        // the newest chain that carried an entry
    size_t                  resent_ = 0;
    size_t                  naks_ = 0;
    size_t                  reliable_pending_ = 0;   // reliable records not ended
};

}}}  // namespace aoc::protocol::repl
