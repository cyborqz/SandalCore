// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/aoc_actor_tracking.h
//
//  AoC's SERVER-TO-SERVER actor-tracking protocol.
//
//  ★★★ MAJOR REVISION (2026-04-27 night, after F5 of sub_14624BD00 + sub_14624D8A0):
//
//  This is NOT a client-facing protocol.  It's the BACKEND CLUSTER protocol
//  that AoC's distributed world servers use to coordinate among themselves:
//    - Server registration (ID 9999/10000): "I'm online, here are my bounds"
//    - Server disconnect (Type 1):          "Server X is going away"
//    - Actor data updates (Type 2):         "Here's the state of actors I own"
//    - Magic 9998:                          End-of-stream marker
//
//  Implication for our emulator:
//    - For the IMMEDIATE goal (single client enters world), this protocol
//      is irrelevant — we are a single backend server, no cluster to coord.
//    - For MULTIPLAYER MOVEMENT (other players visible to a client), the
//      mechanism is different: standard UE5 FastArraySerializer on a
//      CustomDelta UPROPERTY, replicated via normal actor channels.  We
//      already have those decoded (sub_141D432C0 etc.).
//
//  This file remains useful as a record of the cluster-level architecture
//  and as a reference if we ever want to support multi-shard sharding.
//
//  Discovered via IDA RE on AOCClient-Win64-Shipping.exe (2026-04-27).
//  Source-of-truth functions and string addresses are listed below.
//
//  ARCHITECTURE
//  ────────────
//
//      UDP socket :7777
//        ├─ UNetConnection (standard UE5 bunches)         ← standard path
//        └─ FServicePacketRouter<UActorTrackingService>   ← THIS file
//                ├─ Server-side: emits via GetLocationUpdatesForServer
//                └─ Client-side: receives via PollImpl, dispatches by type byte
//
//  WIRE FORMAT
//  ───────────
//
//    Each packet:
//      [1 byte] type ∈ {0,1,2,3} (EActorTrackingPacketType)
//      [body]   per-type payload (see structs below)
//
//    Type 3 has a sub-protocol:
//      [1 byte]  type=3
//      [SIP]     packetId
//      if id == 9998 → end-of-stream / disconnect (magic value)
//      else         → sub-dispatch by id (sub_1462C3740)
//
//  KEY IDA ADDRESSES (AOCClient-Win64-Shipping.exe)
//  ─────────────────────────────────────────────────
//
//    sub_14624B430   FServicePacketRouter<UATSHandler>::PollImpl     (client recv)
//    sub_14624BD00   FServicePacketRouter<UATS>::PollImpl            (server)
//    sub_14625E710   UFilteredActorTrackingRegistry::GetLocationUpdatesForServer
//    sub_14625D390   UFilteredActorTrackingRegistry::GetAllActorLocations
//    sub_1462C5ED0   UFilteredActorTrackingRegistry::UpdateServerRelevancyTable
//    sub_14625D890   FServicePacketRouter<UATS>::GetConnectionFromServerId
//    sub_14624D8A0   FServicePacketRouter<UATS>::ProcessDisconnect_Internal
//    sub_14624D120   FServicePacketRouter<UATSHandler>::ProcessDisconnect_Internal
//    sub_146255990   FServicePacketRouter<UATS>::AttemptFlushSendMetrics
//    sub_146255C10   FServicePacketRouter<UATSHandler>::AttemptFlushSendMetrics
//    sub_1462C3740   Type-3 sub-dispatcher (by packet ID)
//    sub_1462D8940   Type-3 magic-9998 handler
//
//  Packet-type vtables (each 3 slots = 24 bytes):
//    off_14B0C0D28   Type 0 vtable   (smallest, no inheritance)
//    off_14B0C0D40   Common base vtable for types 1/2/3 (64B header)
//      [0] sub_141F30E40   destructor (operator delete 64 B)
//      [1] sub_14625E0E0   ToString (base) — debug-print abstract impl
//      [2] sub_14625F650   ★ Serialize / Process — UNREAD, F5 NEEDED
//    off_14B0C0D58   Type 2 derived vtable (96B Ship object)
//      [0] sub_145029D60   destructor (frees OwnerName FString, op delete 96 B)
//      [1] sub_14625E250   ToString — CONFIRMED writes ShipYawRotation/
//                          OwnerName/MaxShipHealth via sprintf
//      [2] sub_14625F6A0   ★ Serialize / Process — UNREAD, F5 NEEDED
//    off_14B0C0D70   Type 3 derived vtable (72B object with extra qword)
//      [0] sub_141F2B070   destructor (operator delete 72 B)
//      [1] sub_14625DE20   ToString
//      [2] sub_14625F5F0   ★ Serialize / Process — UNREAD, F5 NEEDED
//
//  Vtable contract: 3 slots = { dtor, ToString, Process }
//
//  REVISION (after F5 of sub_14625F6A0): vtable slot 2 is NOT Serialize.
//  It's a state-mutation/promote method — sets vtable to base D28, calls
//  sub_1462519E0 for the actual data copy, then writes type byte at +96.
//  AoC's actor-tracking does NOT use per-class virtual serializers.
//  Wire serialization happens at the FLUSH/BATCH level, not per-object.
//
//  SEMANTIC HINT (string adjacent to D70 in .rdata):
//    "ActiveTargetingState" — confirms this protocol carries combat/targeting
//    state and skill-action data alongside ship state.  Service name
//    "UActorTrackingService" reflects the broader scope.
//
//  ACTUAL MOVEMENT/POSITION DATA flows through a SEPARATE serializer:
//    sub_141C52BB0 — called from GetLocationUpdatesForServer at offset
//    +144 of the 224-byte send-record.  The 96-byte Type-2 (Ship) object
//    is just per-actor METADATA — the position/velocity is in the 80-byte
//    "property delta blob" written by sub_141C52BB0.  F5 sub_141C52BB0
//    to find the actual location wire format.
//
//  Lifecycle function tables (each indexed by type 0..3):
//    funcs_14625EDF1   Constructors  (sub_1458A69A0/B0, sub_146257AF0, sub_1458A6A50)
//    funcs_1458A6C67   Destructors   (sub_141F30E40, sub_145029D60, sub_141F2B070, +1 unknown)
//    funcs_1458A6DE7   Pre-init / move-init (UNREAD — last F5 needed)
//
//  STATUS
//  ──────
//  - Object layouts: ✓ confirmed via destructors (operator delete size constants)
//  - Receive flow:   ✓ decoded
//  - Send flow:      ✓ decoded
//  - Wire format:    ⚠ object SIZES known, but Serialize methods not yet F5'd
//                      (vtable[1+] of D40/D58/D70 — last RE round needed)
//  - 80-byte property delta blob format (sub_141C52BB0): ⚠ not decoded
//
//  SESSION: continued from docs/SESSION-2026-04-27-SAVE.md
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>

namespace aoc { namespace protocol { namespace actor_tracking {

// ─── EActorTrackingPacketType ────────────────────────────────────────────────
// 4-value enum from binary RE.  Read as the first byte of every packet.

// REVISED interpretation (2026-04-27 night, after F5 of sub_14625E250):
// These aren't 4 packet TYPES — they're 4 ACTOR CATEGORIES with their own
// per-actor state data.  Each category has its own vtable with ToString +
// (probably) Serialize.  The actual movement/position data flows separately
// through the 80-byte property-delta blob written by sub_141C52BB0 at
// offset +144 of each 224-byte send-record.
// FINAL revision (after sub_14624BD00 + sub_14624D8A0 F5):
// These are ROUTER OPCODES, not actor categories.  The actor categories
// (Ship/Generic/etc.) live INSIDE Type 2 packet payloads as a sub-tag.
enum class ERouterOpcode : uint8_t {
    // Type 0 — Connection bootstrap / Hello.
    // Handler: vtable[16] of UATSHandler (server side).
    // Reads extra payload via sub_141462460 before dispatch.
    Hello = 0,

    // Type 1 — Server disconnect notification.
    // Handler: sub_14624D8A0 (ProcessDisconnect_Internal).
    // Wire layout: [+0] type=1 [+4] ServerId(uint32) [+8..] payload.
    Disconnect = 1,

    // Type 2 — Bulk actor-data update (Ship/Generic/etc. payloads).
    // Handler: vtable[8] of UATSHandler.
    // Carries one or more per-actor entries; each has its own sub-category
    // byte at body+96 indicating what kind of actor (Ship is one kind).
    ActorBulkUpdate = 2,

    // Type 3 — Magic-ID-routed packet.
    // After type byte: [SIP packetId].  Then:
    //   - 9998:        end-of-stream / disconnect marker
    //   - 9999/10000:  Server registration handshake (reads ServerId+Bounds)
    //   - other:       sub-dispatched via sub_1462C35C0 by ID
    Routed = 3,
};

// ── SERVER REGISTRATION HANDSHAKE PROTOCOL (Type 3, packet ID 9999/10000) ──
// Wire layout per sub_14624BD00 lines 491-494:
//
//   [1 B]   type = 3
//   [SIP]   packetId = 9999 (or 10000 with extra payload)
//   [...]   ServerId   — read by sub_1458B40A0 (likely uint32)
//   [...]   Bounds     — read by sub_1458A9570; FBox-like struct (Min, Max, IsValid)
//                        Local frame holds 3×16B = 48 bytes
//   [...]   Conn info  — read by sub_14504E200 (likely connection metadata)
//   [1 B]   trailing flag byte (read via vtable+392 = SerializeBits)
//
// On receipt the server logs one of:
//   "Registering bounds for new server"
//   "Re-registering previously connected server..."
//   "Server was already registered, but old bounds were not valid / found"
//   "Incoming connection is attempting to register with the connection api,
//    but its server ID already exists in the ServerConnection map.
//    the old one will be overwritten"

// ── DISCONNECT PACKET (Type 1) ──
// Wire layout per sub_14624D8A0:
//
//   [+0]    type byte = 1
//   [+1..3] padding
//   [+4]    ServerId (uint32) — used to look up connection in 3 hash maps
//   [+8..]  optional payload (freed via sub_141620F20 at end)
//
// The 64-byte base header carries the ServerId as part of its 3×16B + 8B
// field set.  Reconnect grace is tracked separately at hashmap a1+888,
// keyed by ServerId.

// ─── Common base header (all type 1/2/3 packets) ─────────────────────────────
// Vtable: off_14B0C0D40
// Size:   64 bytes
//
// Set transiently by types 1/2/3 ctors before the derived vtable overwrites.
// Type 1 keeps this vtable as final.
//
// FIELD TYPES are INFERRED from size + AoC's NetGUID/Vector conventions.
// The 3×16-byte slots are most likely: NetGUID + Location-quantized + Velocity.

struct PacketBaseHeader {
    uint64_t vtable_ptr;          // +0   (set by ctor; client doesn't read this)

    // 3 × 16-byte slots — likely actor identity + transform
    uint8_t  field_a[16];         // +8   likely FIntrepidNetworkGUID (Tracker?)
    uint8_t  field_b[16];         // +24  likely FIntrepidNetworkGUID (Actor?) or Location
    uint8_t  field_c[16];         // +40  likely FVector / FRotator (with padding)

    uint64_t timestamp_or_seq;    // +56  qword — timestamp/sequence
};
static_assert(sizeof(PacketBaseHeader) == 64, "Base must be 64 B");
// [Cyb - C | O | A]

// ─── Type 2 packet (LocationBulk) ────────────────────────────────────────────
// Vtable: off_14B0C0D58
// Size:   96 bytes (12 of which are alignment padding around the TArray)
// Dtor:   sub_145029D60 (frees Data ptr at +72 via sub_1413B0B20)

struct PacketType2_Ship {
    PacketBaseHeader base;        // +0..63
    float    ship_yaw_rotation;   // +64  ★ confirmed via sub_14625E250 ToString
    uint32_t pad0;                // +68  (alignment for FString)
    // FString OwnerName at +72..+87:
    wchar_t* owner_name_data;     // +72  (heap-owned wide string)
    int32_t  owner_name_num;      // +80
    int32_t  owner_name_max;      // +84
    float    max_ship_health;     // +88  ★ confirmed
    uint32_t pad1;                // +92  (alignment to 96 B)
};
static_assert(sizeof(PacketType2_Ship) == 96, "Type2(Ship) must be 96 B");

// ─── Type 3 packet (SingleRef) ───────────────────────────────────────────────
// Vtable: off_14B0C0D70
// Size:   72 bytes
// Dtor:   sub_141F2B070

struct PacketType3 {
    PacketBaseHeader base;        // +0..63
    uint64_t extra_qword;         // +64  single-ref data (NetGUID? offset?)
};
static_assert(sizeof(PacketType3) == 72, "Type3 must be 72 B");

// ─── Internal server-side structures ─────────────────────────────────────────
// These are NOT on the wire — they're the server's runtime registry.
// Sizes confirmed via stride arithmetic in sub_14625E710.

constexpr size_t kFActorInformationBytes        = 80;   // confirmed: 80*v44 stride
constexpr size_t kFActorTrackerRegistrationBytes = 240; // confirmed: 240*v39 stride
constexpr size_t kSendQueueRecordBytes          = 224;  // confirmed: 224*v60 stride

// Send-queue record layout (per sub_14625E710):
//   [+0..39]   3×16B + 8B = base header copied from source
//   [+40..135] type-specific body (96 B max, fits all derived types)
//   [+136]     type byte (EPacketType)
//   [+144..223] 80-byte property delta blob (written by sub_141C52BB0)
//
// Total = 224 bytes per actor update queued for send.

// ─── 9998 magic packet ID ────────────────────────────────────────────────────
// In Type 3 packets, packetId == 9998 triggers special handling
// (sub_1462D8940 — likely connection-end / final-flush marker).

constexpr uint32_t kMagicEndOfStreamPacketId = 9998;

// ─── 80-byte property delta blob struct (sub_141C52BB0 move-ctor) ──────────
// At offset +144 of every 224-byte send-queue record.  Holds the actual
// per-actor delta data (position/velocity/etc.).  Looks like an FBitWriter
// or FNetBitWriter variant with an owned heap buffer.
struct PropertyDeltaBlob {
    uint64_t pad0;            // +0   (init 0)
    uint64_t pad1;            // +8   (init 0)
    uint8_t  reserved_16[16]; // +16..31 (not touched by ctor)
    uint64_t pad2;            // +32  (init 0)
    int32_t  position;        // +40  (init 0) — current bit/index position
    int32_t  limit;           // +44  (= 128 by default — max bits/handles?)
    int32_t  sentinel;        // +48  (= -1, "no current selection")
    int32_t  pad3;            // +52  (init 0)
    int32_t  subobject_id;    // +56  (conditionally copied from source)
    int32_t  pad4;            // +60  (not touched)
    void*    buffer_data;     // +64  ★ owned heap buffer (already filled
                              //         by upstream caller before move-ctor)
    int32_t  buffer_size;     // +72  buffer size in bits or bytes (TBD)
    int32_t  pad5;            // +76  (alignment to 80 B)
};
static_assert(sizeof(PropertyDeltaBlob) == 80, "PropertyDeltaBlob must be 80 B");

// ─── Wire-format encoders (TBD — pending discovery of the actual sender) ──
//
// REVISION (2026-04-27 night, after 5 more F5s):
//
// AoC does NOT use per-class virtual serializers.  The 3-slot vtable is:
//   [0] dtor    [1] ToString    [2] DeepCopy / CloneInto
// (slot 2 wraps the per-type copy ctor like sub_1462519E0 for Ship)
//
// Three function tables exist, all 4 entries indexed by EActorCategory:
//   funcs_14625EDF1[]  — construct (used by GetLocationUpdatesForServer)
//   funcs_1458A6DE7[]  — construct (used by array swap/init in container ops)
//                        Likely the copy-ctor variant.
//   funcs_1458A6C67[]  — destruct
//
// Multiple container record sizes seen:
//   144 B per record (sub_1458A6D40 swap stride) — header + body, no blob
//   160 B per record (sub_1458A9870 init stride) — header + body + sentinels
//   224 B per record (sub_14625E710 queue stride) — header + body + 80B blob
//
// Confirmed: Ship's OwnerName at +72 is a standard UE5 FString
// (sub_1462519E0 calls sub_14133E920 = FString::operator= for deep-copy).
// So PacketType2_Ship layout is correct as defined above.
//
// `sub_146255990` (AttemptFlushSendMetrics) is JUST telemetry — it computes
// avg/min/max packet sizes and logs them. NOT a wire serializer.
//
// Where IS the wire serializer? Hypotheses for tomorrow:
//   1. UIntrepidNetDriver::SendUDPPacket (or similar) — directly memcpys
//      queue records to the wire (zero-copy design).
//   2. FServicePacketRouter::AttemptSend (counterpart to AttemptFlushSendMetrics).
//   3. Inside sub_14624BD00 (PollImpl server side, not yet F5'd).
//
// Easiest find: xref WSAAPI sendto and filter to functions in 0x14624xxx
// or 0x14625xxx range — those are the FServicePacketRouter code segment.

}}} // namespace aoc::protocol::actor_tracking
