// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/replicator.h
//
//  The replicator: an actor with its subobjects (component schema + values +
//  fast arrays), a per-connection shadow of what the client has, and the three
//  things a connection sends for it, each in the original server's order and
//  form, ready for worldd's Connection::Chain:
//
//    open_actor    the open of the actor's channel: the export bunches (the
//                  archetype, the level, the class of a dynamic subobject, each
//                  component written) and the payload
//                    SerializeNewActor                  actor, archetype and level
//                                                       GUIDs, location, rotation,
//                                                       scale, velocity
//                    [PlayerController fields]          NetPlayerIndex(8),
//                                                       ClientHandshakeId(32)
//                    the actor's content block          RepLayout block, fast-array
//                                                       fields (left out when empty)
//                    a content block per subobject      the same; an empty one
//                                                       still has its block
//    update_actor  content blocks for what changed since the shadow: the actor's
//                  and each subobject's changed handles and fast-array deltas;
//                  nothing (nullopt) when nothing did.  A subobject the shadow has
//                  not seen (added to the actor after the open) is sent in full
//                  with its export, as an open would.
//    call_rpc      an RPC on the actor or on one of its subobjects (rpc.h)
//
//  The order is the actor's (Actor::subobjects), which is the order of the
//  original server's blocks: pkt#78 opens with BaseCharacterInfo, CombatInfo,
//  OwnerInfo, Backpack, Equipment, QuestStorage, RewardStorage, Character
//  Appearance; pkt#107 continues with AlignmentComponent first.  Exports come
//  in the order the GUIDs are named.  Chains, partial bunches and sequences are
//  the Connection's: an Emission is the export bunches and the payload of one
//  Chain (Emission::exports / payload are its fields of the same names).
//
//  Every function is all or nothing: on a failure (*failed) the shadow is as it
//  was (an undo record of the entries touched, not a copy of the whole shadow: an
//  update that sends nothing copies nothing).  The pieces are also public
//  (collect_*_blocks, assemble_*), so a caller can replace a block's payload before
//  it goes into the bunch; ShadowUndo is what puts the shadow back when it then
//  cannot send.
//
//  Per-connection conditions.  UE sends a property to a connection only when its
//  replication condition holds for it (OwnerOnly, SkipOwner, SimulatedOnly,
//  AutonomousOnly, InitialOnly ...: the connection's relation to the object).  The
//  hook is ObjectShadow::active, a ParentMask (rep_block.h) the caller computes from
//  the connection's role: the writers leave out the properties of an inactive
//  parent, and when one turns active whatever differs from the shadow is sent.
//  The table that says which property has which condition is NOT built here: it
//  comes from the exe (GetLifetimeReplicatedProps) with plan A5, which decides per
//  property for each connection.
//
//  Not supported: deleting a dynamic subobject.  A block that names a subobject
//  exists only to create or update it; UE's destroy message (bIsDestroyMessage in
//  the content block header, which subobject.h refuses to read) is not written, so
//  a dynamic subobject cannot be taken away from a client that has it (removing
//  it from Actor::subobjects only stops sending it updates).
//
//  GUIDs, paths and checksums are the caller's: the actor and its components
//  come from the per-ServerId allocator (protocol/server_guid_allocator.h), the
//  archetype's, the level's and the components' names and checksums from the
//  content (the capture's, in the tests).
// ============================================================================
#pragma once

#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/subobject.h"
#include "protocol/schema/generated/schema_types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// C O A / C-Y-B

namespace aoc { namespace protocol { namespace repl {

// ---- the objects ---------------------------------------------------------------

/// The replicated values of one object: its class, the values by handle, the fast arrays, and the codec
/// settings its class needs (FRepMovement's rotation quantization, the stat table).
struct ObjectState {
    const schema::ClassSchema* schema = nullptr;   // nullptr: nothing to replicate, an empty block
    RepState                   state;
    FastArrays                 arrays;
    CodecOptions               opts;
};

/// A replicated subobject (a component).  Stably named: `name` and `checksum` name it in the package map.
/// Dynamic: the client creates it from `dynamic_class` (a class export chain).
struct Subobject {
    emit::FIntrepidNetworkGUID guid;
    ObjectState                obj;
    std::string                name;
    uint32_t                   checksum = 0;
    std::optional<ExportChain> dynamic_class;   // set: not stably named
    std::optional<emit::FIntrepidNetworkGUID> outer;   // dynamic only: the outer when it is not the actor
};

/// UPackageMapClient::SerializeNewActor's optional parts.
struct SpawnInfo {
    std::optional<QuantVec> location;   // UE::Net::WriteQuantizedVector, as the captured opens carry it
    std::optional<Rot>      rotation;   // FRotator, compressed shorts
    std::optional<QuantVec> scale;
    std::optional<QuantVec> velocity;
};

/// APlayerController::OnSerializeNewActor (AoC's network version predates the connection identifier).
struct PlayerControllerOpen {
    uint8_t  net_player_index = 0;
    uint32_t client_handshake_id = 0;
};

struct ActorOpen {
    ExportChain                          archetype;   // the archetype and its package: SerializeNewActor's second GUID
    ExportChain                          level;       // the level and its outers: the third
    SpawnInfo                            spawn;
    std::optional<PlayerControllerOpen>  player_controller;
};

/// An actor: a dynamic one (its ObjectId even; UE calls odd ones static, with a path of their own).
struct Actor {
    emit::FIntrepidNetworkGUID guid;
    ObjectState                obj;
    ActorOpen                  open;
    std::vector<Subobject>     subobjects;   // in block order

    Subobject*       find(const emit::FIntrepidNetworkGUID& g);
    const Subobject* find(const emit::FIntrepidNetworkGUID& g) const;
};

// ---- what the connection has ---------------------------------------------------

/// What one connection has of one object.
struct ObjectShadow {
    RepShadow        rep;
    FastArrayShadows fast;
    /// The properties this connection gets (per-connection replication conditions; rep_block.h ParentMask).  Set by the
    /// caller from the connection's role, before the open or any time after.  It is not something that was sent, so an
    /// open (a new channel) keeps it.
    ParentMask       active;
    /// A block for the object has been collected: the client knows it.  A subobject whose shadow is not opened is new and
    /// gets its block, empty or not, with its export.
    bool             opened = false;

    /// Forgets that `handle` was sent: the next update sends it again, whatever its value.  Unexplained: UE has no known
    /// mechanism that resends an unchanged value; the original server did, AuthServerIDReplicated's 0 at pkt#107, and
    /// this reproduces the capture.
    void invalidate(uint32_t handle) { rep.sent.erase(static_cast<uint16_t>(handle)); }

    /// A new channel: the client has nothing of the object.  `active` stays.
    void forget_sent() {
        rep.clear();
        fast.clear();
        opened = false;
    }
};

struct ActorShadow {
    ObjectShadow                actor;
    std::map<uint64_t, ObjectShadow> subobjects;   // by the subobject's ObjectId: one the actor gained is new here

    /// A new channel: the client has nothing.  A subobject's entry goes unless the caller gave it a mask (then it stays,
    /// forgotten).  Entries of subobjects the actor no longer has linger until then (a few KB each): the map is bounded by
    /// the subobjects the actor ever had.
    void forget_sent() {
        actor.forget_sent();
        for (auto it = subobjects.begin(); it != subobjects.end();) {
            if (it->second.active.all_active()) {
                it = subobjects.erase(it);
            } else {
                it->second.forget_sent();
                ++it;
            }
        }
    }
};

/// What collect_*_blocks changed in an ActorShadow, to put back (update_actor / open_actor do it when the assembly fails;
/// a caller that assembles the blocks itself keeps it for the same).  An update records only the entries it touched: one
/// that sends nothing leaves it empty.  An open records the shadow it replaced.
struct ShadowUndo {
    struct Object {
        uint64_t            id = 0;            // a subobject's ObjectId
        bool                is_actor = false;
        bool                created = false;   // the collect made the entry
        bool                was_opened = false;
        RepShadowUndo       rep;
        FastArrayShadowsUndo fast;
    };
    std::vector<Object>        objects;
    std::optional<ActorShadow> whole;   // an open

    /// Puts `shadow` back as it was before the collect.
    void rollback(ActorShadow& shadow) const;
};

/// What one update carried, per object, for a NAK (send_ledger.h): the RepLayout handles written, the fast-array items written or removed, and
/// whether the update was the subobject's first block.  Built from the update's ShadowUndo and the shadow after it (sent_record()).
struct SentObject {
    uint64_t              id = 0;             // a subobject's ObjectId; 0 with is_actor for the actor itself
    bool                  is_actor = false;
    bool                  introduced = false; // the update was the first block the client got of this subobject (it came with its export)
    std::vector<uint16_t> handles;
    struct Fast {
        uint32_t             field = 0;
        std::vector<int32_t> changed;         // ReplicationIDs whose items the payload carried (new or changed)
        std::vector<int32_t> removed;         // ReplicationIDs it deleted
    };
    std::vector<Fast>     fast;
};

struct SentRecord {
    std::vector<SentObject> objects;
    bool empty() const { return objects.empty(); }
};

/// The record of an update: `undo` is what collect_update_blocks recorded (what the update touched), `after` the shadow once it was written.
SentRecord sent_record(const ShadowUndo& undo, const ActorShadow& after);

// ---- what goes out --------------------------------------------------------------

/// One Chain's worth: its export bunches and its payload.
struct Emission {
    std::vector<emit::BunchWriter>          exports;
    emit::BunchWriter                       payload;
    /// Every GUID written into an export bunch with its path (an object, its outer package ...).  Mark them in
    /// ExportAcks only once the packet carrying the export bunch is acknowledged (UPackageMapClient::ReceivedAck /
    /// ReceivedNak: a NAK leaves them unacknowledged, and so does a bunch that is merely queued or sent, even on a reliable
    /// channel): the caller marks `exported_by_bunch[i]` when the export bunch `exports[i]` is acknowledged.
    std::vector<emit::FIntrepidNetworkGUID> exported;
    /// The same per export bunch, parallel to `exports`.
    std::vector<std::vector<emit::FIntrepidNetworkGUID>> exported_by_bunch;
    /// The bunch's reliability: true for an open and an update (worldd's actor channel is reliable) and for RPCs that are
    /// all reliable; false for RPCs that are all unreliable (call_rpcs refuses a mix: one bunch has one reliability).
    bool                                    reliable = true;
};
// C O A.C-Y-B

/// One content block before it goes into the bunch.
struct ContentBlock {
    const Subobject*  sub = nullptr;   // nullptr: the actor
    bool              has_rep_layout = false;
    emit::BunchWriter payload{0};      // the RepLayout block, then the fast-array fields (no buffer until something is written)
    /// Objects the payload names by path (a class, an asset): exported after the subobject's own export,
    /// unless acknowledged.  An RPC's parameters, e.g. ClientSetHUD's HUD class.
    std::vector<ExportChain> exports;
};

/// The blocks of an open: the actor's (when it has anything to send) and one per subobject, an empty one
/// included; the shadow becomes the open's (forget_sent first: the masks stay).  false, shadow as it was, for a value the
/// codecs cannot carry.  `undo` (when given) receives what puts the shadow back should the caller fail to assemble.
bool collect_open_blocks(const Actor& actor, ActorShadow& shadow, std::vector<ContentBlock>& out,
                         ShadowUndo* undo = nullptr);

/// The blocks of an update: what changed on the actor and on its subobjects (a new one in full), for the properties
/// the shadow's masks let through.  Empty `out` when nothing did, and then nothing was copied.  false, shadow as it was,
/// for a value the codecs cannot carry.  `undo` as above.
bool collect_update_blocks(const Actor& actor, ActorShadow& shadow, std::vector<ContentBlock>& out,
                           ShadowUndo* undo = nullptr);

/// SerializeNewActor and the rest of an open, then `blocks`, with the exports they need.  nullopt for an
/// actor that is not dynamic, an archetype or level without a GUID, or an export that does not fit a bunch.
std::optional<Emission> assemble_open(const Actor& actor, const std::vector<ContentBlock>& blocks,
                                      const ExportAcks& acks);

/// `blocks` (not empty) with the exports they need: for each block in turn the component written or the class of a
/// dynamic one, then the block's own `exports`.
std::optional<Emission> assemble_blocks(const Actor& actor, const std::vector<ContentBlock>& blocks,
                                        const ExportAcks& acks);

/// The open of the actor's channel; the shadow is reset first (a new channel: the client has nothing).
std::optional<Emission> open_actor(const Actor& actor, ActorShadow& shadow, const ExportAcks& acks,
                                   bool* failed = nullptr);

/// The update; nullopt when nothing changed (*failed false) or on a failure (*failed true).  `record` (when given) receives what the update
/// carried, for the caller's SendLedger (a lost packet's properties are sent again from the current values, UE's FRepLayout::ReceivedNak).
std::optional<Emission> update_actor(const Actor& actor, ActorShadow& shadow, const ExportAcks& acks,
                                     bool* failed = nullptr, SentRecord* record = nullptr);

/// One RPC call: on the actor (`target` nullptr) or one of its subobjects.
struct RpcRequest {
    const Subobject*         target = nullptr;
    std::string_view         rpc;
    RpcParams                params;
    std::vector<ExportChain> exports;   // the classes and assets the parameters name (ContentBlock::exports)
};

/// Calls, each as a content block of its own in one bunch, in order: the original server batches (pkt#23 chSeq
/// 958 carries three PlayerController RPCs).  Exports: each target's when not acknowledged, then the call's.
/// nullopt for an RPC the target's class does not have, parameters that do not match, an export that does
/// not fit a bunch, a target that is neither the actor nor one of its subobjects (Actor::subobjects), or calls that
/// mix reliable and unreliable RPCs (one bunch has one reliability: Emission::reliable).  Call them apart.
std::optional<Emission> call_rpcs(const Actor& actor, std::span<const RpcRequest> calls, const ExportAcks& acks);

/// One call: `call_rpcs` with a single request.
std::optional<Emission> call_rpc(const Actor& actor, const Subobject* target, std::string_view rpc_name,
                                 const RpcParams& params, std::span<const ExportChain> extra_exports,
                                 const ExportAcks& acks);

}}}  // namespace aoc::protocol::repl
