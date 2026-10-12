// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/pawn_emitter.cpp  —  M1.4.b
//
//  See pawn_emitter.h for rationale.  This implementation wraps
//  captured_pkt78_bunch_stream.h with a narrow send call via
//  IGameServerHost::send_bunch_packet.
// ============================================================================
#include "net/pawn_emitter.h"
#include "net/native_connect_sequencer.h"   // IGameServerHost
#include "net/captured_pkt78_bunch_stream.h" // kCapturedPkt78BunchStream + sizes

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>

namespace aoc { namespace net {

PawnEmitter::PawnEmitter(IGameServerHost& host, const std::string& client_key)
    : host_(host), client_key_(client_key) {}

bool PawnEmitter::emit_captured(const sockaddr_in& client_addr) {
    // PM33 (2026-04-29): emit only bunch[2] (ch=114 reliable ctrl, the
    // Pawn ActorOpen).  Verbose UE5 net log proved bunch[0] (ch=85
    // unreliable update) caused "Received unreliable bunch before open"
    // → cursor desync → CNSF.  See extract_pkt78_bunches_raw.py v4.
    spdlog::warn("[PawnEmitter] Emitting spliced pkt#78 bunch stream "
                 "({} bits / {} bytes, ONLY bunch[2] = ch=114 Pawn ActorOpen)",
                 kCapturedPkt78BunchStreamBits,
                 kCapturedPkt78BunchStreamBytes);

    // send_bunch_packet wraps the raw bunch stream in a new packet with
    // our session's seq/ack/PacketInfo.  The 3 concatenated bunches
    // inside the stream parse sequentially on the client side — same
    // way the client handles any multi-bunch packet.
    bool ok = host_.send_bunch_packet(client_key_, client_addr,
                                        kCapturedPkt78BunchStream,
                                        kCapturedPkt78BunchStreamBits);
    if (!ok) {
        spdlog::error("[PawnEmitter] send_bunch_packet failed");
        return false;
    }
    spdlog::warn("[PawnEmitter] ★ Pawn ActorOpen sent "
                 "(PM33: only ch=114 reliable ctrl bunch[2])");
// (Cyb : C | O | A)

    // Phase B.0p4 (2026-04-28 PM) — fire native ClientRestart RIGHT after
    // pkt#78 ships.
    //
    // Background: with the v3 fix, pkt#78 carries the GUIDExport for
    // NetGUID 88 + the Pawn ActorOpen.  Once the client processes pkt#78,
    // NetGUID 88 is registered in its PackageMap and refers to a live
    // APawn instance.  At that point we can validly send a ClientRestart
    // RPC referencing NetGUID 88, and the client's
    // APlayerController::ClientRestart_Implementation will:
    //   1. Set Pawn = ResolvedPawn
    //   2. Call AcknowledgePossession(Pawn)
    //   3. Set AcknowledgedPawn = Pawn
    //   4. Send ServerAcknowledgePossession back to us
    //
    // sub_146B00200's "AAoCPlayerController - No valid pawn" check reads
    // AcknowledgedPawn — once non-null, the loading screen hides.
    //
    // We previously gated this on SNLW (ServerNotifyLoadedWorld) detection
    // in the C>S handler, but the client only sends SNLW twice in our test
    // session and BOTH arrived before pkt#78 was emitted (deferring
    // forever).  Firing right after the pkt#78 send guarantees correct
    // ordering: pkt#78 → ClientRestart → AcknowledgePossession.
    //
    // ── Phase B.0p13 (2026-04-28 PM13) — fuzz DISABLED, ClientRestart skipped ──
    //
    // PM12 fuzzed 18 candidate handles for ClientRestart RPC dispatch with
    // BARE NetGUID payload.  ZERO C>S Server* response RPCs.  Conclusive
    // proof: AOC does NOT use the field-handle-based ClientRestart RPC
    // dispatch path for possession.
    //
    // AOC's possession appears to be PROPERTY-DRIVEN via PC.Pawn (or
    // PawnPrivate, per allPawn includes.txt RE).  The captured PC chain
    // (pkts 22-46) replicates PC.Pawn = NetGUID 88 and SHOULD trigger the
    // pawn-ack handshake on the client.  If that's not happening, the
    // remaining suspect is captured-packet bit-misalignment causing
    // property replication to fail (CNSFs 1744830464 + 318997300 burst at
    // IntrepidInitialize moment).
    //
    // NEXT INVESTIGATION (separate change):
    //   1. Find which captured packet produces the 318997300 garbage
    //      value when re-emitted (likely another splice with bit
    //      misalignment similar to pkt#78 v1's bug).
    //   2. Extract bunch_start_bit per-bunch (not just per-packet) so
    //      partial-bunch chains splice cleanly.
    //   3. OR construct PC.Pawn property update natively post-pkt#78
    //      to force the client's OnRep_Pawn → ack handshake.
    //
    // For this build, pkt#78 ships and we don't fire a native ClientRestart
    // — just rely on the captured property replication in pkts 22-46.
    spdlog::info("[PawnEmitter] (PM13: native ClientRestart fuzz disabled — "
                 "captured PC chain handles possession via Pawn property OnRep)");
    return true;
}

}} // namespace aoc::net
