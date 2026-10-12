// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/world_bootstrap_emitter.cpp
//
//  Implementation of the Road A WorldBootstrapEmitter.  See header for
//  architecture rationale.  This file is intentionally short — the
//  per-mode logic delegates to the existing native emitters
//  (BootstrapEmitter / PcEmitter / PawnEmitter) and to the host's
//  send_captured_packet primitive for splice rows.
// ============================================================================
#include "net/world_bootstrap_emitter.h"
#include "net/captured_identity.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/wire/packet_parser.h"
#include "net/native_connect_sequencer.h"   // IGameServerHost
#include "net/bootstrap_emitter.h"
#include "net/pc_emitter.h"
#include "net/pawn_emitter.h"
#include "net/player_pawn_emitter.h"        // PM39 (2026-04-30) Phase 2
#include "net/player_pawn_splicer.h"         // PM106 (2026-05-04) Phase D Step 2.2
#include "net/appearance_emitter.h"          // PM103 (2026-05-04) Phase D Step 2
#include "net/player_state_emitter.h"        // PD3 (2026-05-05) Phase D Step 3

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace aoc { namespace net {

namespace {
// AAoCPlayerController::CharacterLoadTracker as the original server sent it
// (protocol/emit/net_serializers.h): before possession and once loaded.
constexpr const char* kLoadTrackerBeforePossession = "1011111111111111111111111";
constexpr const char* kLoadTrackerLoaded           = "0111111111111111111111111";
}  // namespace

WorldBootstrapEmitter::WorldBootstrapEmitter(IGameServerHost& host,
                                              const std::string& client_key)
    : host_(host), client_key_(client_key) {}

bool WorldBootstrapEmitter::emit_all(
    const sockaddr_in& addr,
    const std::vector<PacketEmissionSpec>& plan)
{
    spdlog::warn("[WorldBootstrap] === begin ({} entries in plan) ===",
                  plan.size());

    const uint32_t loaded_replay = host_.loaded_replay_packet_count();
    if (loaded_replay == 0) {
        spdlog::warn("[WorldBootstrap] WARNING: no replay loaded — Splice "
                      "entries will fail.  Pure-native paths only will run.");
    } else {
        spdlog::warn("[WorldBootstrap] replay holds {} packets", loaded_replay);
    }

    size_t emitted = 0, splice_ok = 0, splice_fail = 0;
    size_t native_ok = 0, native_fail = 0, skipped = 0;

    for (size_t i = 0; i < plan.size(); ++i) {
        const auto& spec = plan[i];
// C-O-A with C-Y-B

        // Pacing — sleep before emit, not after, so consecutive Skips
        // don't accumulate latency for the next real emit.
        if (spec.delay_ms_before > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(spec.delay_ms_before));
        }

        const bool ok = dispatch_one(addr, spec);
        switch (spec.mode) {
        case EmissionMode::Skip:
            ++skipped; break;
        case EmissionMode::Splice:
            (ok ? ++splice_ok : ++splice_fail); break;
        case EmissionMode::NativePc0:
        case EmissionMode::NativePc22:
        case EmissionMode::NativePawn78:
            (ok ? ++native_ok : ++native_fail); break;
        }
        if (ok) ++emitted;

        // Periodic progress log (every 25 entries to avoid spam)
        if ((i + 1) % 25 == 0) {
            spdlog::info("[WorldBootstrap] progress {}/{} (splice {}/{}, "
                          "native {}/{}, skipped {})",
                          i + 1, plan.size(),
                          splice_ok, splice_ok + splice_fail,
                          native_ok, native_ok + native_fail, skipped);
        }
    }

    spdlog::warn("[WorldBootstrap] === complete: emitted={} splice={}/{} "
                  "native={}/{} skipped={} ===",
                  emitted, splice_ok, splice_ok + splice_fail,
                  native_ok, native_ok + native_fail, skipped);

    // Phase B.0e2 (2026-04-27) — let the host drain any post-bootstrap work
    // (e.g., queued SULV ACKs that were deferred to avoid colliding with
    // mid-splice captured chSeq values).  Default impl is a no-op.
    host_.on_world_bootstrap_complete(client_key_, addr);

    return true;  // Soft errors don't abort the whole bootstrap
}

bool WorldBootstrapEmitter::dispatch_one(const sockaddr_in& addr,
                                          const PacketEmissionSpec& spec)
{
    switch (spec.mode) {
    case EmissionMode::Skip:
        // Sentinel/keepalive — Maintain handles these naturally.
        spdlog::debug("[WorldBootstrap]   [skip] idx={} '{}'",
                      spec.replay_idx, spec.description ? spec.description : "");
        return true;

    case EmissionMode::Splice:
        return host_.send_captured_packet(client_key_, addr, spec.replay_idx);

    case EmissionMode::SpliceChannel:
        return splice_channel(addr, spec.replay_idx, spec.channel);

    case EmissionMode::NativePc0: {
        BootstrapEmitter em(host_, client_key_);
        // For now BootstrapEmitter::emit_all is one call (opcode-3 + the
        // welcome no-op); ideally we'd split it, but it's already minimal.
        return em.emit_all(addr);
    }

    case EmissionMode::NativePc22: {
        // PcEmitter handles both ActorOpen (pkt#22 equivalent) and the
        // initial Name property update (pkt#~104 equivalent).  We keep
        // them paired here because emit_properties depends on the actor
        // channel being open from emit_open.
        PcEmitter pc(host_, client_key_);
        if (!pc.emit_open(addr)) {
            spdlog::error("[WorldBootstrap] NativePc22: emit_open failed");
            return false;
        }
        // Small inter-bunch gap so the client processes the open before
        // the property update arrives.
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        if (!pc.emit_properties(addr)) {
            // Non-fatal: PC is open, just custom-name failed
            spdlog::warn("[WorldBootstrap] NativePc22: emit_properties "
                          "failed (PC still open)");
        }
        // What the original server sent next on the PC channel (replay
        // pkt#22-23): ClientSetHUD, then voice / level streaming / time.
        if (pc.emit_client_set_hud(addr, pc_ch_seq_)) ++pc_ch_seq_;
        pc_ch_seq_ += pc.emit_bootstrap_rpcs(addr, pc_ch_seq_);
        // Character load progress as it stood before possession (pkt#72).
        if (pc.emit_load_tracker(addr, pc_ch_seq_, kLoadTrackerBeforePossession)) ++pc_ch_seq_;
        return true;
    }

    case EmissionMode::NativePawn78: {
        PawnEmitter pe(host_, client_key_);
        return pe.emit_captured(addr);
    }

    case EmissionMode::NativePlayerPawn: {
        if (use_captured_player_identity()) {
            // The pawn with its components, as captured (net/captured_identity.h):
            // the pawn channel's whole chain, then possession and load
            // completion on the PC channel, then reveal the pawn.
            for (uint32_t pkt = kCapturedPawnFirstPkt; pkt <= kCapturedPawnLastPkt; ++pkt)
                if (!splice_channel(addr, pkt, kCapturedPawnChannel)) return false;
            PcEmitter pc(host_, client_key_);
            if (pc.emit_pawn_link(addr, pc_ch_seq_)) {
                ++pc_ch_seq_;
                if (pc.emit_load_tracker(addr, pc_ch_seq_, kLoadTrackerLoaded)) ++pc_ch_seq_;
            }
            PlayerPawnEmitter ppe(host_, client_key_);
            ppe.emit_hidden(addr, kCapturedPawnChannel, kCapturedPawnNextChSeq, /*hidden=*/false);
            return true;
        }

        // PM39 (2026-04-30) — Path C Phase 2: native player Pawn ActorOpen.
        // Distinct from NativePawn78 above (which spliced a captured Guard
        // NPC).  This emits a fresh Pawn actor on its own channel with a
        // server-minted NetGUID for the connected player.
        PlayerPawnEmitter ppe(host_, client_key_);
        if (!ppe.emit_open(addr)) return false;

        // ── PM106 (2026-05-04) — Phase D Step 2.2: pkt#78 verbatim splice ──
        //
        // Ship the captured pkt#78 (full 3-bunch inner stream, 5160 bits)
        // AFTER our synthetic PlayerPawnEmitter has opened the player Pawn
        // channel.  This registers the captured archetype + level paths in
        // the client's PackageMap and creates a SECOND pawn (NetGUID 54)
        // alongside our minted one (NetGUID 16777218).
        //
        // Goal: validate that the captured pawn appears with a visible
        // mesh (proves AOC mesh assembly works given the right replicated
        // data).  If yes, next step is to either possess the captured pawn
        // OR rewrite the captured Pawn NetGUID to match our minted one.
        //
        // Gated on probe_pkt78_splice.txt (default disabled).  Failure is
        // non-fatal — possession still works via the synthetic path.
        //
        // We send the splice BEFORE the PC.Pawn link below so the splice's
        // ch=0 control message bunch doesn't compete with our PC channel
        // updates that follow.  ch=85 GUIDExport carries the archetype
        // path registration that the synthetic Pawn already emitted, but
        // duplicate registrations are harmless to the client's PackageMap.
        PlayerPawnSplicer pps(host_, client_key_);
        if (!pps.emit_captured_stream(addr)) {
            spdlog::warn("[WorldBootstrap] pkt#78 splice failed "
                         "(non-fatal — synthetic path still active)");
        }

        // ── PM53 (2026-04-30) — Phase C: link PC.Pawn → AcknowledgePossession ──
        //
        // Now that the Pawn actor is registered on the client (RegisterNetGUID
        // confirmed in test logs at line 3999, channel 19 IsDynamic:1), we send
        // a property update on the PC channel telling the client our PC's
        // .Pawn = <our minted Pawn NetGUID>.  Client's OnRep_Pawn then fires
        // AcknowledgePossession() — camera attaches, input routes, player is
        // in-world and controllable.
        //
        // Failure here is non-fatal: Pawn channel is open, just orphaned from
        // the PC.  Log warn and proceed; user will iterate cmd_handle if needed.
        PcEmitter pc(host_, client_key_);
        if (pc.emit_pawn_link(addr, pc_ch_seq_)) {
            ++pc_ch_seq_;
            // Character load complete (pkt#229): the client starts moving
            // its pawn only once this arrives.
            if (pc.emit_load_tracker(addr, pc_ch_seq_, kLoadTrackerLoaded)) ++pc_ch_seq_;
        } else {
            spdlog::warn("[WorldBootstrap] PC.Pawn link emission failed "
                         "(non-fatal — Pawn channel is open but unpossessed)");
        }
// == CoA | C-Y-B ==

        // PM103 (2026-05-04) — Phase D Step 2: appearance seed (Option A test).
        //
        // Fire the AppearanceEmitter AFTER possession completes.  Currently
        // sends a near-no-op bunch (single ForceHideHeldItems=1 property at
        // a placeholder handle).  Goal: prove the property update pipeline
        // reaches CharacterAppearanceComponent end-to-end.  If this bunch
        // is accepted without errors, we have the infrastructure to send
        // real appearance data once we extract the property RepIndices and
        // struct layouts from the captured replay (Phase D Step 2.1).
        //
        // Failure here is non-fatal — possession is already complete.
        AppearanceEmitter app(host_, client_key_);
        if (!app.emit_default_seed(addr)) {
            spdlog::warn("[WorldBootstrap] Appearance seed emission failed "
                         "(non-fatal — possession is already established)");
        }

        // Phase D Step 3 (2026-05-05) — PlayerState emission.
        //
        // After possession + appearance, emit the PlayerState ActorOpen
        // carrying real character identity (PlayerName, CharacterArchetype,
        // etc.) so the client's mesh assembler sees who the player is.
        // Probe-gated; default disabled (no-op success).
        //
        // Iter2: ActorOpen on ch=21 chSeq=954 (just channel registration).
        // Iter3: PlayerName property update on ch=21 chSeq=955 immediately
        //        after the open lands.
        PlayerStateEmitter ps(host_, client_key_);
        if (!ps.emit_open(addr)) {
            spdlog::warn("[WorldBootstrap] PlayerState ActorOpen failed "
                         "(non-fatal)");
        }
        if (!ps.emit_player_name(addr)) {
            spdlog::warn("[WorldBootstrap] PlayerName update failed "
                         "(non-fatal)");
        }
        // Iter4: link PC.PlayerState → our minted PS NetGUID so the
        // nameplate widget re-binds to OUR PS (where we sent PlayerName).
        // Reuses pc but only fires when probe_player_state_emit.txt = 1.
        if (!pc.emit_player_state_link(addr)) {
            spdlog::warn("[WorldBootstrap] PC.PlayerState link failed "
                         "(non-fatal)");
        }

        // ── PM147 (2026-05-08) — World Partition cell keepalive ──────────
        //
        // After possession + PS link, send ClientUpdateLevelStreamingStatus
        // to keep World Partition cells alive past the GC sweep that fires
        // when the loading screen drops.  Probe-gated via
        // probe_streaming_keepalive.txt (default off).  See pc_emitter.cpp
        // for the full RE rationale.
        if (!pc.emit_client_update_level_streaming_status(addr)) {
            spdlog::warn("[WorldBootstrap] streaming keepalive failed "
                         "(non-fatal)");
        }

        // ── 2026-05-05 — ClientInitializeCharacter() RPC ──────────────
        //
        // After possession (PC.Pawn link via ClientRestart) and PS
        // wiring, fire `ClientInitializeCharacter()` to tell the client
        // to run its local character init (SetRace, SetGender, populate
        // RaceGenderAppearanceId).  Without this, OnRep_CharacterCustomization
        // arrives but the appearance subobject has no race-specific
        // DataTable to look up, so the merge produces nothing.
        //
        // Wire handle 142 per docs/RE-CLIENTINITIALIZECHARACTER-HANDLE.md
        // (DERIVED-FROM-RE ±10).  Probe-override via probe_cic_handle.txt.
        //
        // Probe knob: probe_cic_emit.txt — set to 1 to enable.  Default off:
        // the original server never sends ClientInitializeCharacter in the
        // captured bootstrap (tools/replay_rpcs), and its field is 173, not the
        // handle 142 the host path guesses.
        bool cic_enabled = false;
        if (std::FILE* fp = std::fopen("probe_cic_emit.txt", "r")) {
            int v = 0;
            std::fscanf(fp, "%d", &v);
            std::fclose(fp);
            cic_enabled = (v != 0);
        }
        if (cic_enabled) {
            if (!host_.emit_client_initialize_character(client_key_, addr)) {
                spdlog::warn("[WorldBootstrap] ClientInitializeCharacter() "
                             "emit failed (non-fatal)");
            }
        } else {
            spdlog::info("[WorldBootstrap] ClientInitializeCharacter "
                          "skipped (probe_cic_emit.txt=0)");
        }

        return true;
    }
    }
// [C.O.A + Cyb]

    spdlog::error("[WorldBootstrap] unknown EmissionMode {} for idx {}",
                  static_cast<int>(spec.mode), spec.replay_idx);
    return false;
}

bool WorldBootstrapEmitter::splice_channel(const sockaddr_in& addr, uint32_t replay_idx,
                                           uint32_t channel) {
    // A channel no native emitter uses keeps its captured reliable sequence,
    // so its bunches can go out unchanged, one per packet, while the other
    // channels sharing the captured packet are left out.
    const std::vector<uint8_t>* raw = host_.captured_packet_raw(replay_idx);
    if (!raw) {
        spdlog::warn("[WorldBootstrap] SpliceChannel: replay packet {} not loaded", replay_idx);
        return true;  // soft failure
    }
    auto parsed = aoc::protocol::wire::parse_packet(raw->data(), raw->size(),
                                                    aoc::protocol::wire::Direction::ServerToClient);
    if (!parsed) {
        spdlog::warn("[WorldBootstrap] SpliceChannel: replay packet {} does not parse", replay_idx);
        return true;
    }
    for (const auto& b : parsed->bunches) {
        if (b.channel != channel) continue;
        const size_t start = b.data_start_bit - b.header_bits;
        const size_t bits = b.header_bits + b.bunch_data_bits;
        aoc::protocol::emit::BunchWriter bw;
        bw.write_bit_range(raw->data(), start, bits);
        if (!host_.send_bunch_packet(client_key_, addr, bw.data(), bits)) return false;
    }
    return true;
}

}} // namespace aoc::net
