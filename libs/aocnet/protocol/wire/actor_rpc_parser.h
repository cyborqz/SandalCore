// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/wire/actor_rpc_parser.h
//
//  Contract-level wire parser for UE5 / IntrepidNet Actor & SubObject RPC bunches.
//
//  Structure of an RPC bunch:
//    1. ContentBlock Header:
//       - bHasRepLayout (1 bit) == 0 (for RPCs)
//       - bIsActor      (1 bit): 1 = root actor (Pawn), 0 = SubObject
//       - If SubObject:
//           IntrepidNet 128-bit NetGUID [ObjectId:32, ObjectId_Hi:32, ServerId:32, Randomizer:32]
//       - cb_payload_bits (SerializeIntPacked / SIP)
//    2. Field Header:
//       - fn_idx        (8 bits, FieldNetIndex in ClassNetCache)
//       - inner_size    (SIP, bit length of RPC parameters)
//    3. RPC Parameters:
//       - parameter payload bits (inner_size)
// ============================================================================
#pragma once

#include "protocol/wire/ue5_primitives.h"
#include <cstdint>
#include <string>
#include <vector>

namespace aoc { namespace protocol { namespace wire {

struct FAoCAbilityTriggerParams {
    uint32_t guid_a = 0;
    uint32_t guid_b = 0;
    uint32_t guid_c = 0;
    uint32_t guid_d = 0;
    int32_t  client_instance_id = 0;
    int32_t  animation_index = 0;
    bool     b_pvp_force_flag = false;

    std::string guid_string() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%08X-%08X-%08X-%08X",
                      guid_a, guid_b, guid_c, guid_d);
        return std::string(buf);
    }
};

struct FAoCAbilityEndParams {
    int32_t server_instance_id = 0;
};

struct FAoCHitVerificationParams {
    std::string target_component;
    uint64_t    ability_guid = 0;
    uint64_t    hit_guid = 0;
    uint64_t    full_ability_record = 0;
    uint64_t    full_hit_record = 0;
};

struct FAoCInteractParams {
    bool     b_has_guid = false;
    uint32_t obj_lo = 0;
    uint32_t obj_hi = 0;
    uint32_t server_id = 0;
    uint32_t randomizer = 0;
    int16_t  counter = 0;
    uint16_t zone_id = 0;

    uint64_t target_guid() const {
        return static_cast<uint64_t>(obj_lo) | (static_cast<uint64_t>(obj_hi) << 32);
    }
};
// [C.Y.B x C O A]

struct FActorRpcBunch {
    bool     b_is_actor = false;
    uint64_t target_guid = 0;       // ObjectId (or 0 for actor root)
    uint32_t server_id = 0;
    uint32_t randomizer = 0;
    uint64_t cb_payload_bits = 0;
    size_t   cb_payload_start = 0;   // first payload bit; the next block starts at + cb_payload_bits
    uint32_t fn_idx = 0;
    size_t   param_bit_offset = 0;
    size_t   param_bit_count = 0;
    std::vector<uint8_t> param_bytes;

    // Structured parameters for known RPCs
    FAoCAbilityTriggerParams  trigger_params;
    FAoCAbilityEndParams      end_params;
    FAoCHitVerificationParams hit_params;
    FAoCInteractParams        interact_params;
    uint64_t                  dialogue_record_id = 0;

    /// Human-readable RPC function name hint based on target & fn_idx
    std::string function_name_hint() const {
        if (b_is_actor) {
            if (fn_idx == 177) return "PlayerPawn::ClientSetupOrActivation";
            if (fn_idx == 178) return "PlayerPawn::ServerInteractWithInteractableComponent";
            if (fn_idx == 424) return "AAoCPlayerController::ServerChooseDialogueOption";
            if (fn_idx == 834) return "AAoCPlayerController::ServerSelectDialogueChoice";
            if (fn_idx == 44)  return "ACharacter::ServerMoveNoBase";
            if (fn_idx == 40)  return "ACharacter::ServerMove";
            return "PlayerPawn::Fn_" + std::to_string(fn_idx);
        } else {
            // SubObject 10341564 is UAoCAbilityComponent
            if (fn_idx == 188) return "UAoCAbilityComponent::ServerTriggerAbility";
            if (fn_idx == 216) return "UAoCAbilityComponent::ServerEndAbility";
            if (fn_idx == 246) return "UAoCAbilityComponent::RequestHitVerification_Server";
            return "SubObject_" + std::to_string(target_guid) + "::Fn_" + std::to_string(fn_idx);
        }
    }
};

inline bool parse_actor_rpc_bunch(const uint8_t* data, size_t data_len,
                                 size_t bunch_data_start, size_t bunch_data_bits,
                                 FActorRpcBunch& out_rpc, uint32_t ch_idx = 0) {
    if (bunch_data_bits < 18 || (bunch_data_start + bunch_data_bits) > (data_len * 8)) {
        return false;
    }

    size_t bit_off = bunch_data_start;
    size_t bunch_end = bunch_data_start + bunch_data_bits;

    // 1. ContentBlock Header: bHasRepLayout (1b), bIsActor (1b)
    uint64_t b_has_rep = ue5::read_bits(data, data_len, bit_off, 1);
    uint64_t b_is_actor = ue5::read_bits(data, data_len, bit_off, 1);
    if (b_has_rep != 0) {
        return false; // RepLayout blocks are property replication, not RPCs
    }

    out_rpc.b_is_actor = (b_is_actor == 1);

    if (out_rpc.b_is_actor) {
        // Target is channel root actor (PlayerPawn or PlayerController)
        out_rpc.target_guid = 0;
        out_rpc.server_id = 0;
        out_rpc.randomizer = 0;
    } else {
        // SubObject: IntrepidNet 128-bit NetGUID [ObjectId:32, ObjectId_Hi:32, ServerId:32, Randomizer:32]
        if (bit_off + 128 > bunch_end) {
            return false;
        }
        uint32_t obj_lo = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 32));
        uint32_t obj_hi = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 32));
        out_rpc.target_guid = static_cast<uint64_t>(obj_lo) | (static_cast<uint64_t>(obj_hi) << 32);
        out_rpc.server_id   = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 32));
        out_rpc.randomizer  = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 32));
    }

    if (bit_off >= bunch_end) return false;

    // ContentBlock payload size (SIP)
    out_rpc.cb_payload_bits = ue5::read_sip(data, data_len, bit_off);
    out_rpc.cb_payload_start = bit_off;
    if (out_rpc.cb_payload_bits == 0 || bit_off + 8 > bunch_end) {
        return false;
    }
// (Cyb : C | O | A)

    // 2. Field Header:
    // On Channel 3 (AAoCPlayerController), wire handle is serialized via SerializeInt(MAX=1025).
    // On Channel 19/9, fn_idx is 8 bits.
    if (ch_idx == 3) {
        out_rpc.fn_idx = static_cast<uint32_t>(ue5::read_serialize_int(data, data_len, bit_off, 1025));
    } else {
        out_rpc.fn_idx = static_cast<uint32_t>(ue5::read_bits(data, data_len, bit_off, 8));
    }

    out_rpc.param_bit_offset = bit_off;
    out_rpc.param_bit_count = (bunch_end > bit_off) ? (bunch_end - bit_off) : 0;

    // Pack parameter bits into bytes
    size_t n_bytes = (out_rpc.param_bit_count + 7) / 8;
    out_rpc.param_bytes.assign(n_bytes, 0);
    size_t cur_bit = bit_off;
    for (size_t i = 0; i < out_rpc.param_bit_count; ++i) {
        uint8_t bit = static_cast<uint8_t>(ue5::read_bits(data, data_len, cur_bit, 1));
        out_rpc.param_bytes[i / 8] |= (bit << (i % 8));
    }

    // Unpack known function parameters:
    if (out_rpc.b_is_actor) {
        if (out_rpc.fn_idx == 178) {
            // PlayerPawn::ServerInteractWithInteractableComponent
            // Field Header: fn_idx (8 bits) + inner_size (SIP)
            // Param 0: Interactable (UInteractableComponent*)
            //          b_has_guid (1 bit), 128-bit NetGUID [obj_lo:32, obj_hi:32, srv:32, rnd:32]
            // Param 1: Counter (2 bits / int16)
            // Param 2: ZoneID (2 bits / uint16)
            size_t p_off = bit_off;
            uint32_t inner_size = static_cast<uint32_t>(ue5::read_sip(data, data_len, p_off));
            (void)inner_size;
            if (p_off < bunch_end) {
                out_rpc.interact_params.b_has_guid = (ue5::read_bits(data, data_len, p_off, 1) != 0);
                if (out_rpc.interact_params.b_has_guid && (p_off + 128 <= bunch_end)) {
                    out_rpc.interact_params.obj_lo = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
                    out_rpc.interact_params.obj_hi = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
                    out_rpc.interact_params.server_id = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
                    out_rpc.interact_params.randomizer = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
                }
                if (p_off < bunch_end) {
                    size_t rem = bunch_end - p_off;
                    if (rem >= 2) {
                        out_rpc.interact_params.counter = static_cast<int16_t>(ue5::read_bits(data, data_len, p_off, std::min<size_t>(rem, 16)));
                    }
                    if (p_off < bunch_end) {
                        size_t rem2 = bunch_end - p_off;
                        out_rpc.interact_params.zone_id = static_cast<uint16_t>(ue5::read_bits(data, data_len, p_off, std::min<size_t>(rem2, 16)));
                    }
                }
            }
        } else if (out_rpc.fn_idx == 424 || out_rpc.fn_idx == 834) {
            // AAoCPlayerController::ServerChooseDialogueOption (424) / ServerSelectDialogueChoice (834)
            size_t p_off = bit_off;
            uint32_t inner_size = static_cast<uint32_t>(ue5::read_sip(data, data_len, p_off));
            (void)inner_size;
            if (p_off + 64 <= bunch_end) {
                out_rpc.dialogue_record_id = ue5::read_bits(data, data_len, p_off, 64);
            }
        }
    } else {
        if (out_rpc.fn_idx == 188 && out_rpc.param_bit_count >= 192) {
            // UAoCAbilityComponent::ServerTriggerAbility
            // Param 0: AbilityGuid (128 bits / 4x 32-bit uint)
            // Param 1: ClientInstanceId (32-bit int)
            // Param 2: AnimationIndex (32-bit int)
            size_t p_off = bit_off;
            out_rpc.trigger_params.guid_a = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
            out_rpc.trigger_params.guid_b = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
            out_rpc.trigger_params.guid_c = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
            out_rpc.trigger_params.guid_d = static_cast<uint32_t>(ue5::read_bits(data, data_len, p_off, 32));
            out_rpc.trigger_params.client_instance_id = static_cast<int32_t>(ue5::read_bits(data, data_len, p_off, 32));
            out_rpc.trigger_params.animation_index = static_cast<int32_t>(ue5::read_bits(data, data_len, p_off, 32));
        } else if (out_rpc.fn_idx == 216 && out_rpc.param_bit_count >= 32) {
            // UAoCAbilityComponent::ServerEndAbility
            // Param 0: ServerInstanceId (32-bit int)
            size_t p_off = bit_off;
            out_rpc.end_params.server_instance_id = static_cast<int32_t>(ue5::read_bits(data, data_len, p_off, 32));
        } else if (out_rpc.fn_idx == 246 && out_rpc.param_bytes.size() >= 32) {
            // UAoCAbilityComponent::RequestHitVerification_Server
            // Extract component name and AoC 48-bit Record GUIDs
            const auto& pb = out_rpc.param_bytes;
            for (size_t i = 0; i + 3 < pb.size(); ++i) {
                if (pb[i] >= 'A' && pb[i] <= 'Z') {
                    size_t j = i;
                    while (j < pb.size() && (std::isalnum(static_cast<unsigned char>(pb[j])) || pb[j] == '_')) ++j;
                    if (j < pb.size() && pb[j] == '\0' && (j - i) >= 3 && (j - i) <= 32) {
                        out_rpc.hit_params.target_component.assign(
                            reinterpret_cast<const char*>(&pb[i]), j - i);
                        break;
                    }
                }
            }
            for (size_t i = 0; i + 8 <= pb.size(); ++i) {
                if (pb[i+6] == 0x29 && pb[i+7] == 0x54) {
                    uint64_t rec = 0;
                    std::memcpy(&rec, &pb[i], 8);
                    uint64_t g = (rec >> 16) & 0xFFFFFFFFFFFFULL;
                    if (out_rpc.hit_params.full_ability_record == 0) {
                        out_rpc.hit_params.full_ability_record = rec;
                        out_rpc.hit_params.ability_guid = g;
                    } else if (out_rpc.hit_params.full_hit_record == 0) {
                        out_rpc.hit_params.full_hit_record = rec;
                        out_rpc.hit_params.hit_guid = g;
                        break;
                    }
                }
            }
        }
    }

    return true;
}

}}} // namespace aoc::protocol::wire
