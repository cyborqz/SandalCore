// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/opcode_dispatcher.cpp
//
//  The state machine implementation.  See opcode_dispatcher.h for the
//  architectural rationale.
// ============================================================================
#include "net/opcode_dispatcher.h"
#include "world/events/world_events.h"
#include <spdlog/spdlog.h>

namespace aoc { namespace net {

OpcodeDispatcher::OpcodeDispatcher(SessionRegistry& sessions,
                                    protocol::NetGuidAllocator* netguid_alloc,
                                    world::events::EventBus* event_bus)
    : sessions_(sessions), netguid_alloc_(netguid_alloc), event_bus_(event_bus) {}

// ─── Top-level dispatch ────────────────────────────────────────────────────

DispatchResult OpcodeDispatcher::dispatch(const DispatchPacket& pkt) {
    if (pkt.client_key.empty()) {
        DispatchResult r;
        r.error = "empty client_key";
        return r;
    }

    // Handshake-initial is the only op that IMPLICITLY creates a session.
    // Everything else requires an existing session; if none exists the
    // dispatcher rejects — real wire-format would treat pre-handshake
    // traffic as garbage.
    ClientSession* cs = nullptr;
    if (pkt.op == DispatchOp::HANDSHAKE_INITIAL) {
        cs = sessions_.get_or_create(pkt.client_key);
    } else {
        cs = sessions_.get(pkt.client_key);
        if (!cs) {
            stats_.rejected_missing_session++;
            DispatchResult r;
            r.error = "no session for " + pkt.client_key + " (op=" +
                      std::to_string(static_cast<int>(pkt.op)) + ")";
            return r;
        }
    }

    cs->touch();
    cs->packets_received++;

    switch (pkt.op) {
        case DispatchOp::HANDSHAKE_INITIAL:
            return handle_handshake_initial(*cs, pkt);
        case DispatchOp::HANDSHAKE_CHALLENGE_RESPONSE:
            return handle_handshake_response(*cs, pkt);

        case DispatchOp::NMT_HELLO:
            return handle_nmt_hello(*cs, pkt);
        case DispatchOp::NMT_LOGIN:
            return handle_nmt_login(*cs, pkt);
        case DispatchOp::NMT_WELCOME:
            return handle_nmt_welcome(*cs, pkt);
        case DispatchOp::NMT_JOIN:
            return handle_nmt_join(*cs, pkt);
        case DispatchOp::NMT_ABORT:
            return handle_nmt_abort(*cs, pkt);
        case DispatchOp::NMT_GAMESPECIFIC:
            return handle_nmt_game_specific(*cs, pkt);
// -- C.O.A . Cyb --

        case DispatchOp::ACTOR_MOVEMENT:
            return handle_actor_movement(*cs, pkt);
        case DispatchOp::ACTOR_RPC:
            return handle_actor_rpc(*cs, pkt);

        case DispatchOp::CLIENT_DISCONNECT:
            return handle_client_disconnect(*cs, pkt);

        // Ops we accept but don't drive the state machine with yet.
        case DispatchOp::NMT_NETSPEED:
        case DispatchOp::NMT_DEBUGTEXT:
        case DispatchOp::NMT_SKIP:
        case DispatchOp::NMT_NETGUIDASSIGN:
        case DispatchOp::NMT_ACTORCHANNELFAIL: {
            // Log + pass through without phase change.
            DispatchResult r = make_accept(*cs, cs->phase);
            r.phase_changed = false;
            return r;
        }

        default:
            stats_.unknown_op++;
            DispatchResult r;
            r.error = "unknown op " + std::to_string(static_cast<int>(pkt.op));
            r.old_phase = r.new_phase = cs->phase;
            return r;
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

DispatchResult OpcodeDispatcher::make_reject(ClientSession& cs, const std::string& reason) {
    stats_.rejected_wrong_phase++;
    DispatchResult r;
    r.accepted = false;
    r.error = reason;
    r.old_phase = cs.phase;
    r.new_phase = cs.phase;
    r.phase_changed = false;
    return r;
}

DispatchResult OpcodeDispatcher::make_accept(ClientSession& cs, ClientPhase before) {
    DispatchResult r;
    r.accepted = true;
    r.old_phase = before;
    r.new_phase = cs.phase;
    r.phase_changed = (before != cs.phase);
    return r;
}

// ─── Handshake handlers ─────────────────────────────────────────────────────

DispatchResult OpcodeDispatcher::handle_handshake_initial(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.handshake_initial++;

    ClientPhase before = cs.phase;
    // First contact — must be either AWAITING_HANDSHAKE (new connection)
    // or HANDSHAKE_IN_PROGRESS (retransmit of Initial due to packet loss).
    if (cs.phase != ClientPhase::AWAITING_HANDSHAKE &&
        cs.phase != ClientPhase::HANDSHAKE_IN_PROGRESS) {
        return make_reject(cs, "handshake_initial in phase " +
                               std::string(to_string(cs.phase)));
    }
    cs.transition_to(ClientPhase::HANDSHAKE_IN_PROGRESS);
    return make_accept(cs, before);
}

DispatchResult OpcodeDispatcher::handle_handshake_response(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.handshake_response++;

    ClientPhase before = cs.phase;
    if (cs.phase != ClientPhase::HANDSHAKE_IN_PROGRESS) {
        return make_reject(cs, "handshake_response in phase " +
                               std::string(to_string(cs.phase)));
    }
    // Real code would verify the HMAC cookie here.  Session E trusts input.
    cs.transition_to(ClientPhase::NMT_NEGOTIATING);
    return make_accept(cs, before);
}

// ─── NMT handlers ───────────────────────────────────────────────────────────

DispatchResult OpcodeDispatcher::handle_nmt_hello(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.nmt_hello++;

    ClientPhase before = cs.phase;
    // NMT_Hello is the first NMT; we accept it once we've handshaked.
    if (cs.phase != ClientPhase::NMT_NEGOTIATING) {
        return make_reject(cs, "nmt_hello in phase " +
                               std::string(to_string(cs.phase)));
    }
    // Stay in NMT_NEGOTIATING; a real server would also emit NMT_Welcome
    // (or NMT_Upgrade on mismatch) via the packet emitter here.  Session F.
    return make_accept(cs, before);  // phase unchanged
}

DispatchResult OpcodeDispatcher::handle_nmt_login(
    ClientSession& cs, const DispatchPacket& pkt) {
    stats_.nmt_login++;

    ClientPhase before = cs.phase;
    if (cs.phase != ClientPhase::NMT_NEGOTIATING) {
        return make_reject(cs, "nmt_login in phase " +
                               std::string(to_string(cs.phase)));
    }
    if (pkt.str_arg.empty()) {
        // Can't authenticate an empty player name.  Real code emits NMT_Failure.
        return make_reject(cs, "nmt_login with empty player name");
    }

    cs.player_name = pkt.str_arg;
    cs.online_id   = pkt.str_arg2;

    // Allocate the per-player NetGUID block if an allocator was wired up.
    // (Tests may omit this to keep the state machine focused.)
    if (netguid_alloc_) {
        cs.netguid_block           = netguid_alloc_->allocate_player_block(cs.client_key);
        cs.pc_netguid              = cs.netguid_block.player_controller;
        cs.pawn_netguid            = cs.netguid_block.pawn;
        cs.player_state_netguid    = cs.netguid_block.player_state;
    }

    cs.transition_to(ClientPhase::AUTHENTICATED);
    return make_accept(cs, before);
}

DispatchResult OpcodeDispatcher::handle_nmt_welcome(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.nmt_welcome++;

    ClientPhase before = cs.phase;
    // UE5 semantics: NMT_Welcome is S→C usually, but can come back C→S as an
    // ack after the client finished loading.  For our state machine, seeing
    // NMT_WELCOME here means "advance to LOADING_MAP" (we just sent it).
    if (cs.phase != ClientPhase::AUTHENTICATED) {
        return make_reject(cs, "nmt_welcome in phase " +
                               std::string(to_string(cs.phase)));
    }
    cs.transition_to(ClientPhase::LOADING_MAP);
    return make_accept(cs, before);
}

DispatchResult OpcodeDispatcher::handle_nmt_join(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.nmt_join++;
// [CoA :: C-Y-B]

    ClientPhase before = cs.phase;
    if (cs.phase != ClientPhase::LOADING_MAP) {
        return make_reject(cs, "nmt_join in phase " +
                               std::string(to_string(cs.phase)));
    }
    cs.transition_to(ClientPhase::SPAWNING);
    // Session F: trigger actor spawn chain here.  Session G: also emit a
    // ActorSpawned event to broadcast to other clients.
    return make_accept(cs, before);
}

DispatchResult OpcodeDispatcher::handle_nmt_abort(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.nmt_abort++;

    ClientPhase before = cs.phase;
    // NMT_Abort is valid from any phase.
    cs.transition_to(ClientPhase::DISCONNECTING);
    return make_accept(cs, before);
}

DispatchResult OpcodeDispatcher::handle_nmt_game_specific(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.nmt_game_specific++;

    ClientPhase before = cs.phase;
    // Session H.1.1 — state-machine tolerance for AoC-specific flow.
    //
    // The original Session E plan had the UE5-ideal progression
    //   NMT_Login → AUTHENTICATED → NMT_Welcome → LOADING_MAP →
    //                NMT_Join    → SPAWNING     → NMT_GameSpecific → IN_WORLD
    //
    // Real-client observation (log emu-20260422-175838.log) shows AoC:
    //   - SKIPS NMT_Welcome (which is S→C anyway, never arrives C→S)
    //   - SKIPS NMT_Join     (AoC uses opcode 18 as its own "ready" signal)
    //   - Sends NMT 18 right after NMT_Netspeed, while session is still
    //     in AUTHENTICATED phase
    //
    // Accept this directly: any of AUTHENTICATED / LOADING_MAP / SPAWNING
    // advances to IN_WORLD.  Already IN_WORLD stays there (server-bound
    // chat, etc.).  Anything earlier is still a phase-mismatch reject.
    switch (cs.phase) {
        case ClientPhase::AUTHENTICATED:
        case ClientPhase::LOADING_MAP:
        case ClientPhase::SPAWNING:
            cs.transition_to(ClientPhase::IN_WORLD);
            return make_accept(cs, before);
        case ClientPhase::IN_WORLD:
            return make_accept(cs, before);  // no phase change
        default:
            return make_reject(cs, "nmt_game_specific in phase " +
                                   std::string(to_string(cs.phase)));
    }
}

// ─── Actor-channel handlers ─────────────────────────────────────────────────

DispatchResult OpcodeDispatcher::handle_actor_movement(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.actor_movement++;

    ClientPhase before = cs.phase;
    // Movement packets are only valid once the client is in-world.
    if (cs.phase != ClientPhase::IN_WORLD) {
        return make_reject(cs, "actor_movement in phase " +
                               std::string(to_string(cs.phase)));
    }
    // Session F: decode location/rotation/velocity, validate speed, push
    // into the simulation's ActorRegistry.  Here we just confirm the
    // bookkeeping.
    return make_accept(cs, before);  // no phase change
}

DispatchResult OpcodeDispatcher::handle_actor_rpc(
    ClientSession& cs, const DispatchPacket& pkt) {
    stats_.actor_rpc++;

    ClientPhase before = cs.phase;
    // RPC packets are valid once the client is in-world.
    if (cs.phase != ClientPhase::IN_WORLD) {
        return make_reject(cs, "actor_rpc in phase " +
                               std::string(to_string(cs.phase)));
    }

    if (pkt.fn_idx == 188) {
        stats_.ability_trigger++;
        cs.active_ability_guid_str = pkt.ability_guid;
        cs.active_ability_instance_id = pkt.instance_id;
        cs.active_ability_anim_index = pkt.anim_index;
        cs.total_abilities_triggered++;
        spdlog::info("[Ability] Client '{}' TRIGGER ability guid={} inst={} anim={} (total={})",
                     cs.client_key, pkt.ability_guid, pkt.instance_id, pkt.anim_index,
                     cs.total_abilities_triggered);
    } else if (pkt.fn_idx == 216) {
        stats_.ability_end++;
        cs.total_abilities_ended++;
        spdlog::info("[Ability] Client '{}' END ability server_inst={} (total={})",
                     cs.client_key, pkt.instance_id, cs.total_abilities_ended);
    } else if (pkt.fn_idx == 246) {
        stats_.hit_verification++;
        cs.total_hits_verified++;
        cs.total_hits_confirmed++;
        cs.last_verified_ability_guid = pkt.u64_arg;
        cs.last_verified_hit_guid = pkt.hit_guid;
        cs.last_verified_hit_component = pkt.hit_component;
        spdlog::info("[Ability] Client '{}' VERIFY HIT comp='{}' ability=0x{:x} hit=0x{:x} (verified={}, confirmed={})",
                     cs.client_key, pkt.hit_component, pkt.u64_arg, pkt.hit_guid,
                     cs.total_hits_verified, cs.total_hits_confirmed);
    }
// C_Y_B and C.O.A

    spdlog::debug("[Dispatcher] Player RPC: client='{}' target=0x{:x} fn_idx={} payload_bytes={}",
                  cs.client_key, pkt.u64_arg, pkt.fn_idx, pkt.payload.size());
    return make_accept(cs, before);  // no phase change
}

// ─── Lifecycle ──────────────────────────────────────────────────────────────

DispatchResult OpcodeDispatcher::handle_client_disconnect(
    ClientSession& cs, const DispatchPacket& pkt) {
    (void)pkt;
    stats_.client_disconnect++;

    ClientPhase before = cs.phase;
    // Any phase can lead to disconnect.  If we had a netguid block, release
    // it so a future connection can reuse the slot.
    if (netguid_alloc_ && cs.netguid_block.is_valid()) {
        netguid_alloc_->release_player_block(cs.client_key);
    }
    cs.transition_to(ClientPhase::DISCONNECTING);
    return make_accept(cs, before);
}

}} // namespace aoc::net
