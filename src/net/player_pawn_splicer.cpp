// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/player_pawn_splicer.cpp  —  PM106 (verbatim) + PM109 (surgical)
//
//  Two splice modes, selected by probe_pkt78_splice.txt:
//
//    "0" or absent → disabled (no-op)
//    "1"           → VERBATIM splice (kCapturedPkt78FullStream, 5160 bits)
//                    Pawn NetGUID stays at the captured value 54.  Creates
//                    a SECOND pawn alongside our minted 16777218.
//    "2"           → SURGICAL splice (kCapturedPkt78SubstitutedStream, 5184 bits)
//                    Pawn NetGUID has been bit-level-rewritten to 16777218
//                    so the captured bunch[2] now binds to OUR Pawn channel
//                    -- the captured 8 subobjects (BaseCharacterInfo,
//                    CombatInfo, OwnerInfo, BackpackComponent,
//                    EquipmentComponent, QuestStorageComponent,
//                    RewardStorageComponent, CharacterAppearanceComponent)
//                    attach to OUR possessed pawn → mesh assembly should
//                    fire on the ALREADY-POSSESSED pawn rather than a
//                    bystander.
//
//  Surgical substitution offline tool: substitute_pkt78_pawn_guid.py
//    1. Reads inner stream (5160 bits)
//    2. Locates Pawn NetGUID at stream bit 2197 (= bunch[2] payload bit 0):
//         captured packed-int = 108 (val=54, dyn=0, 8 bits)
//    3. Re-encodes new packed-int = 33554437 (val=16777218, dyn=1, 32 bits)
//    4. Stream grows by +24 bits (5160 → 5184)
//    5. Updates BunchDataBits in bunch[2] header (2963 → 2987) at the
//       13-bit SerializeInt position right before the payload (stream bit 2184)
//    6. Emits kCapturedPkt78SubstitutedStream with verified bits.
//
//  Caveats: ONLY the Pawn NetGUID itself is substituted in this pass.  If
//  the captured stream's 8 subobjects reference Pawn=54 as their outer
//  NetGUID inside their InternalLoadObject blocks, those references still
//  point to 54 in the substituted stream — the client may store the
//  subobjects under a (54-rooted) path while the actor is at 16777218.
//  Whether the client tolerates that mismatch is the test we're running.
// ============================================================================
#include "net/player_pawn_splicer.h"
#include "net/native_connect_sequencer.h"               // IGameServerHost
#include "net/captured_pkt78_full_stream.h"             // verbatim
#include "net/captured_pkt78_substituted_stream.h"      // surgical (PM109)
#include "net/captured_pkt78_v3_channel_substituted_stream.h"  // PM130 (channel surgery)

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>
#include <cstdio>

namespace aoc { namespace net {

PlayerPawnSplicer::PlayerPawnSplicer(IGameServerHost& host,
                                      const std::string& client_key)
    : host_(host), client_key_(client_key) {}
// [C.O.A :: Cyb]

namespace {

/// Read probe_pkt78_splice.txt → returns 0 (disabled), 1 (verbatim), 2 (surgical).
int read_splice_mode() {
    std::FILE* fp = std::fopen("probe_pkt78_splice.txt", "r");
    if (!fp) return 0;
    int v = 0;
    std::fscanf(fp, "%d", &v);
    std::fclose(fp);
    return v;
}

}  // namespace

bool PlayerPawnSplicer::emit_captured_stream(const sockaddr_in& client_addr) {
    const int mode = read_splice_mode();
    if (mode == 0) {
        spdlog::info("[PlayerPawnSplicer] disabled "
                     "(probe_pkt78_splice.txt absent or \"0\")");
        return true;  // no-op success
    }

    if (mode == 1) {
        spdlog::warn("[PlayerPawnSplicer] PM106 VERBATIM mode — "
                     "shipping captured pkt#78 unchanged "
                     "({} bits / {} bytes, captured Pawn NetGUID = 54)",
                     kCapturedPkt78FullStreamBits,
                     kCapturedPkt78FullStreamBytes);

        const bool ok = host_.send_bunch_packet(
            client_key_, client_addr,
            kCapturedPkt78FullStream, kCapturedPkt78FullStreamBits);
        if (!ok) {
            spdlog::error("[PlayerPawnSplicer] send_bunch_packet failed (verbatim)");
            return false;
        }
        spdlog::warn("[PlayerPawnSplicer] ★ pkt#78 VERBATIM splice sent "
                     "(Pawn NetGUID 54 -- coexists with our minted 16777218)");
        return true;
    }

    if (mode == 2) {
        spdlog::warn("[PlayerPawnSplicer] PM109 SURGICAL mode — "
                     "shipping pkt#78 with Pawn NetGUID rewritten to {} "
                     "({} bits / {} bytes, +24b vs verbatim)",
                     kCapturedPkt78SubstitutedPawnNetGuid,
                     kCapturedPkt78SubstitutedStreamBits,
                     kCapturedPkt78SubstitutedStreamBytes);
        spdlog::warn("[PlayerPawnSplicer]   Bit-level surgery applied at "
                     "stream bit 2197 (bunch[2] payload start): "
                     "8b packed-int(54) → 32b packed-int(16777218 dyn=1)");
        spdlog::warn("[PlayerPawnSplicer]   BunchDataBits in bunch[2] header "
                     "updated 2963 → 2987 to compensate the +24-bit shift");

        const bool ok = host_.send_bunch_packet(
            client_key_, client_addr,
            kCapturedPkt78SubstitutedStream, kCapturedPkt78SubstitutedStreamBits);
        if (!ok) {
            spdlog::error("[PlayerPawnSplicer] send_bunch_packet failed (surgical)");
            return false;
        }
        spdlog::warn("[PlayerPawnSplicer] ★ pkt#78 SURGICAL splice sent — "
                     "captured 8 subobjects should now bind to NetGUID 16777218");
        return true;
    }

    if (mode == 3) {
        spdlog::warn("[PlayerPawnSplicer] PM130 CHANNEL+GUID SURGICAL mode — "
                     "shipping pkt#78 with Pawn NetGUID rewritten to 16777218 "
                     "AND channel rewritten 114 -> {} "
                     "({} bits / {} bytes)",
                     kCapturedPkt78V3ChanSubstChIdx,
                     kCapturedPkt78V3ChanSubstStreamBits,
                     kCapturedPkt78V3ChanSubstStreamBytes);
        spdlog::warn("[PlayerPawnSplicer]   bunch[2] header bit 1647 (SIP ChIdx): "
                     "captured 114 -> our ch=19, so the captured Pawn ActorOpen "
                     "binds to OUR PlayerPawnEmitter's already-open ch=19 channel.");
// CoA / C:Y:B

        const bool ok = host_.send_bunch_packet(
            client_key_, client_addr,
            kCapturedPkt78V3ChanSubstStream, kCapturedPkt78V3ChanSubstStreamBits);
        if (!ok) {
            spdlog::error("[PlayerPawnSplicer] send_bunch_packet failed (PM130)");
            return false;
        }
        spdlog::warn("[PlayerPawnSplicer] ★ pkt#78 PM130 channel-surgical splice sent");
        return true;
    }

    spdlog::warn("[PlayerPawnSplicer] unknown probe_pkt78_splice.txt value: {} "
                 "(use 0=disabled, 1=verbatim, 2=surgical, 3=channel-surgical)", mode);
    return true;
}

}}  // namespace aoc::net
