// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/emit/replayout/catalog.cpp
//
//  STUB — to be filled in with real property data from the IDA dumps.
//
//  Current state: every catalog returns an empty ClassCatalog with only
//  the name set.  Compilation works, but any caller that tries to
//  encode/decode will fall back to the RawBits path (which is exactly
//  what we want while the catalogs are unpopulated — round-trip stays
//  bit-identical).
//
//  POPULATION ORDER (as IDA dumps arrive):
//    1. aactor_catalog()                — stock UE5, small (~9 props)
//    2. acontroller_catalog()           — stock UE5, tiny (~2 props)
//    3. aplayer_controller_catalog()    — stock UE5 + AoC overrides
//    4. aaoc_player_controller_catalog  — 19 props we already have
//    5. aplayer_state_catalog           — stock UE5 baseline
//    6. aaoc_player_state_catalog       — CharacterArchetype likely here
//    7. apawn_catalog, acharacter_catalog, aaoc_pawn_catalog (for pkt#78)
//
//  LAYER:  Protocol / emit / replayout
// ============================================================================
#include "protocol/emit/replayout/catalog.h"

namespace aoc { namespace protocol { namespace emit { namespace replayout {

uint32_t ClassCatalog::total_cmd_count() const {
    uint32_t n = parent ? parent->total_cmd_count() : 0;
    for (const auto& p : own_props) {
        n += 1;
        if (p.type == FPropertyType::Struct) {
            n += static_cast<uint32_t>(p.sub_cmds.size());
        }
    }
    return n;
}

const ReplicatedPropertyDesc*
ClassCatalog::property_at_cmd(uint32_t cmd_index) const {
    // Walk parents first — they occupy the lower cmd_index range.
    if (parent) {
        const uint32_t parent_count = parent->total_cmd_count();
        if (cmd_index < parent_count) {
            return parent->property_at_cmd(cmd_index);
        }
        cmd_index -= parent_count;
    }
    uint32_t running = 0;
    for (const auto& p : own_props) {
        if (running == cmd_index) return &p;
        ++running;
        if (p.type == FPropertyType::Struct) {
            for (const auto& sub : p.sub_cmds) {
                if (running == cmd_index) return &sub;
                ++running;
            }
        }
    }
    return nullptr;
}

const ReplicatedPropertyDesc*
ClassCatalog::property_by_name(const std::string& name) const {
    for (const auto& p : own_props) {
        if (p.name == name) return &p;
    }
    return parent ? parent->property_by_name(name) : nullptr;
}
// C Y B with CoA

// ── Catalog singletons (STUB) ───────────────────────────────────────────
// Each returns a static-local ClassCatalog.  Until the IDA dumps are
// translated into actual entries, all own_props vectors are empty.

namespace {

const ClassCatalog& make_stub_catalog(const char* name,
                                       const ClassCatalog* parent = nullptr) {
    // This function returns a *reference* to a static-local.  Each unique
    // `name` gets its own storage because we wrap each accessor below.
    static ClassCatalog c;
    c.class_name = name;
    c.parent = parent;
    return c;
}

} // namespace

const ClassCatalog& aactor_catalog() {
    static ClassCatalog c;
    c.class_name = "AActor";
    c.parent = nullptr;

    // ── REPLICATED PROPERTIES (13 total, post-2026-04-24 correction) ───
    //
    // Populated from DIRECT BINARY RE of AOCClient-Win64-Shipping.exe on
    // 2026-04-24.  Walked the FPropertyParams pointer table at VA
    // 0x14A77DB70 (93 entries total) and filtered to those with the
    // CPF_Net (0x20) bit set in PropertyFlags.
    //
    // Previously this list had 15 entries including bIsInterServerReplicated
    // and ProxyNetUpdateInterval, both AoC additions.  BINARY RE proved
    // those two do NOT have CPF_Net set — they use AoC's bit-63 flag
    // (0x8000000000000000, which we're calling CPF_InterServer for now)
    // instead.  They replicate via a different pathway (server-to-server
    // InterServer channel) and NEVER appear in pkt#22's RepLayout stream.
    //
    // Confirmed CPF flag for each (from PropertyFlags field at +0x10):
    //   AuthServerIDReplicated : 0x0040000000000020  CPF_Net only
    //   bReplicateMovement     : 0x0040000100010021  CPF_Net|CPF_RepNotify
    //   bHidden                : 0x4040000200000035  CPF_Net
    //   bTearOff               : 0x0040000000000020  CPF_Net
    //   bCanBeDamaged          : 0x0040000001000025  CPF_Net
    //   bReplicates            : 0x4020080100010035  CPF_Net|CPF_RepNotify
    //   ReplicatedMovement     : 0x4040040100010021  CPF_Net|CPF_RepNotify
    //   RemoteRole             : 0x4040000000022821  CPF_Net
    //   AttachmentReplication  : 0x4020088100002020  CPF_Net|CPF_RepNotify
    //   Owner                  : 0x0114000100000020  CPF_Net|CPF_RepNotify
    //   Role                   : 0x0040000000020821  CPF_Net
    //   NetDormancy            : 0x4010000100010035  CPF_Net|CPF_RepNotify
    //   Instigator             : 0x4145000100000024  CPF_Net|CPF_RepNotify
    //
    // RepIndex order matches declaration order in the FPropertyParams
    // pointer table — this IS the stream order in pkt#22.
    //
    // Struct expansion (ReplicatedMovement, AttachmentReplication) is
    // not filled in yet — those sub_cmds will be populated when we have
    // FStruct decoder support.
    if (c.own_props.empty()) {
        auto add = [&](uint32_t rep_idx, const char* name,
                        FPropertyType t) {
            ReplicatedPropertyDesc d;
            d.rep_index = rep_idx;
            d.name = name;
            d.type = t;
            c.own_props.push_back(std::move(d));
        };
        //     RepIdx  Name                       Type
        //     ------  ------------------------   -----------------------
        // AuthServerIDReplicated is a 128-bit FIntrepidNetworkGUID (Object
        // type on the wire), NOT a 32-bit FInt.  The FPropertyParams
        // TypeCode=0x0084 we observed is likely a size-hint or class
        // discriminator, not the primitive-type code.  Setting type=Int
        // causes the decoder to consume only 32 bits, leaving 96 bits of
        // garbage in the stream which collides with the next cmd_index read.
        //   Round-trip evidence (2026-04-24): with type=Int, the decoder
        //   saw "cmd_index=0 twice" because the middle of the 128-bit GUID
        //   happened to be zeros.  With type=Object, body=128 bits, and
        //   the decoder advances to the next real cmd_index correctly.
        add( 0, "AuthServerIDReplicated",   FPropertyType::Object);   // FIntrepidNetworkGUID (128-bit)
        add( 1, "bReplicateMovement",       FPropertyType::Bool);
        add( 2, "bHidden",                  FPropertyType::Bool);
        add( 3, "bTearOff",                 FPropertyType::Bool);
        add( 4, "bCanBeDamaged",            FPropertyType::Bool);
        add( 5, "bReplicates",              FPropertyType::Bool);
        add( 6, "ReplicatedMovement",       FPropertyType::Struct);   // FRepMovement (~13 sub-cmds)
        add( 7, "RemoteRole",               FPropertyType::Byte);     // ENetRole enum (uint8)
        add( 8, "AttachmentReplication",    FPropertyType::Struct);   // FRepAttachment
        add( 9, "Owner",                    FPropertyType::Object);   // AActor*
        add(10, "Role",                     FPropertyType::Byte);     // ENetRole enum (uint8)
        add(11, "NetDormancy",              FPropertyType::Byte);     // ENetDormancy enum (uint8)
        add(12, "Instigator",               FPropertyType::Object);   // APawn*

        // NOTE: bIsInterServerReplicated and ProxyNetUpdateInterval are
        // AoC additions BUT have CPF_InterServer (bit 63 = 0x8000000000000000)
        // instead of CPF_Net — they replicate via server-to-server channel,
        // not the RepLayout stream.  DO NOT ADD to this list.
    }
    return c;
}

const ClassCatalog& acontroller_catalog() {
    static ClassCatalog c;
    c.class_name = "AController";
    c.parent = &aactor_catalog();

    // Cluster at 0x14A877A60..0x14A877BA0.  2 replicated properties.
    if (c.own_props.empty()) {
        auto add = [&](uint32_t rep_idx, const char* name,
                        FPropertyType t) {
            ReplicatedPropertyDesc d;
            d.rep_index = rep_idx;
            d.name = name;
            d.type = t;
            c.own_props.push_back(std::move(d));
        };
        add(0, "PlayerState", FPropertyType::Object);  // APlayerState* (OnRep_PlayerState)
        add(1, "Pawn",        FPropertyType::Object);  // APawn*        (OnRep_Pawn)
    }
    return c;
}

const ClassCatalog& aplayer_controller_catalog() {
    static ClassCatalog c;
    c.class_name = "APlayerController";
    c.parent = &acontroller_catalog();

    // Cluster at 0x14AA40720..0x14AA41930.  2 replicated properties.
    if (c.own_props.empty()) {
        auto add = [&](uint32_t rep_idx, const char* name,
                        FPropertyType t) {
            ReplicatedPropertyDesc d;
            d.rep_index = rep_idx;
            d.name = name;
            d.type = t;
            c.own_props.push_back(std::move(d));
        };
        add(0, "TargetViewRotation", FPropertyType::Struct);  // FRotator
        add(1, "SpawnLocation",      FPropertyType::Struct);  // FVector_NetQuantize
    }
    return c;
}
// CoA.C-Y-B

const ClassCatalog& aaoc_player_controller_catalog() {
    static ClassCatalog c;
    c.class_name = "AAoCPlayerController";
    c.parent = &aplayer_controller_catalog();

    // ── REPLICATED PROPERTIES (19 total) ────────────────────────────────
    //
    // Ground truth from DIRECT BINARY RE of AOCClient-Win64-Shipping.exe
    // on 2026-04-24.  Walked the FPropertyParams pointer table at VA
    // 0x14B6D5410 (224 total entries in AAoCPlayerController) and filtered
    // to those with the CPF_Net (0x20) bit set.  Confirmed 19 matches,
    // exactly matching the count we had inferred.
    //
    // Declaration order IS RepIndex order within AAoCPlayerController's
    // own slice of the hierarchy.
    //
    // Flags observed per property (CPF bitmap at FPropertyParams +0x10):
    //   RepIdx  Name                           PropertyFlags        RN  IS
    //   ------  -----------------------------  -------------------- --  --
    //   0       bRegisteredForDamageMeter      0x4010000100002020   Y   -
    //   1       CaravanLaunchNode              0x8010000100000020   Y   Y
    //   2       SocketDebugData                0x0010000000000020   -   -
    //   3       CurrentSurveyingScanResults    0x8040000100000020   Y   Y
    //   4       CurrentSurveyingSearchResults  0x8040000100000020   Y   Y
    //   5       bEnableVehicleRecovery         0x8040000100000020   Y   Y
    //   6       VehicleRecoveryTransform       0x8040000100000020   Y   Y
    //   7       ControllersOriginalPawn        0x4144000000000020   -   -
    //   8       ControlledExternalPawn         0x0044000000000020   -   -
    //   9       CharacterLoadTracker           0x8040000000000020   -   Y
    //   10      SummonCooldownTimer            0x0040000000000020   -   -
    //   11      Name                           0x4040000100002020   Y   -
    //   12      DefaultRespawnInfo             0x0040000000000020   -   -
    //   13      CurrentCommissionBoard         0x8040000100000020   Y   Y
    //   14      PuppetComponentReference       0x4144000000080029   -   -
    //   15      CharacterInGameSettings        0x4040000100002020   Y   -
    //   16      MarkedTargets                  0x0040000100000020   Y   -
    //   17      CalloutQueueReplication        0x0010000000002020   -   -
    //   18      CurrentDialogueInstance        0x0010000100000020   Y   -
    //
    // RN = CPF_RepNotify (has OnRep_ callback)
    // IS = bit 63 set (AoC's CPF_InterServer — replicates both client AND
    //      to other AoC backend servers)
    //
    // Type inferences:
    //   ✓  CONFIRMED by TypeCode=0x0001 (bool bitfield) or observed wire data
    //   ?  BEST-GUESS from property name; Unknown uses RawBits fallback
    if (c.own_props.empty()) {
        auto add = [&](uint32_t rep_idx,
                        const char* name,
                        FPropertyType t,
                        uint32_t offset = 0) {
            ReplicatedPropertyDesc d;
            d.rep_index = rep_idx;
            d.name = name;
            d.type = t;
            d.offset_in_instance = offset;
            c.own_props.push_back(std::move(d));
        };
        //     RepIdx  Name                           Type              Offset
        //     ------  -----------------------------  ----------------  ---------
        add(0,  "bRegisteredForDamageMeter",    FPropertyType::Bool,    0x3A60); // ✓ TypeCode=0x0001
        add(1,  "CaravanLaunchNode",            FPropertyType::Object,  0x1B28); // Object (class+1B28 byte ofs)
        add(2,  "SocketDebugData",              FPropertyType::Unknown, 0x23E0); // ? struct or array
        add(3,  "CurrentSurveyingScanResults",  FPropertyType::Array,   0x2F88); // array (plural)
        add(4,  "CurrentSurveyingSearchResults",FPropertyType::Array,   0x2FE8); // array (plural)
        add(5,  "bEnableVehicleRecovery",       FPropertyType::Bool,    0x3A60); // ✓ TypeCode=0x0001
        add(6,  "VehicleRecoveryTransform",     FPropertyType::Struct,  0x30D0); // FTransform
        add(7,  "ControllersOriginalPawn",      FPropertyType::Object,  0x3148); // APawn*
        add(8,  "ControlledExternalPawn",       FPropertyType::Object,  0x3150); // APawn*
        add(9,  "CharacterLoadTracker",         FPropertyType::Unknown, 0x3158); // ? obj or struct
        add(10, "SummonCooldownTimer",          FPropertyType::Float,   0x3174); // float ("Timer" suffix)
        add(11, "Name",                         FPropertyType::String,  0x3178); // ✓ FString (RandomChar in wire)
        add(12, "DefaultRespawnInfo",           FPropertyType::Struct,  0x3188); // struct
        add(13, "CurrentCommissionBoard",       FPropertyType::Object,  0x31A8); // UCommissionBoard*
        add(14, "PuppetComponentReference",     FPropertyType::Object,  0x33E0); // UComponent*
        add(15, "CharacterInGameSettings",      FPropertyType::Struct,  0x33E8); // struct
        add(16, "MarkedTargets",                FPropertyType::Array,   0x34B8); // array (plural + RepNotify)
        add(17, "CalloutQueueReplication",      FPropertyType::Struct,  0x34F8); // struct
        add(18, "CurrentDialogueInstance",      FPropertyType::Object,  0x37A8); // UDialogueInstance*
    }
    return c;
}
// C.Y.B and CoA

const ClassCatalog& aplayer_state_catalog() {
    static ClassCatalog c;
    c.class_name = "APlayerState";
    c.parent = &aactor_catalog();   // APlayerState inherits directly from AActor

    // Populated from ida_dump_parent_classes.idc on 2026-04-23.
    // Cluster at 0x14AA48B88..0x14AA48FC0.  10 replicated properties.
    if (c.own_props.empty()) {
        auto add = [&](uint32_t rep_idx, const char* name,
                        FPropertyType t) {
            ReplicatedPropertyDesc d;
            d.rep_index = rep_idx;
            d.name = name;
            d.type = t;
            c.own_props.push_back(std::move(d));
        };
        //     RepIdx  Name                 Type (stock UE5)
        //     ------  -------------------  --------------------
        add(0, "Score",                FPropertyType::Float);   // float (OnRep_Score)
        add(1, "PlayerId",             FPropertyType::Int);     // int32 (OnRep_PlayerId)
        add(2, "bIsSpectator",         FPropertyType::Bool);
        add(3, "bOnlySpectator",       FPropertyType::Bool);
        add(4, "bIsABot",              FPropertyType::Bool);
        add(5, "bIsInactive",          FPropertyType::Bool);    // (OnRep_bIsInactive)
        add(6, "bFromPreviousLevel",   FPropertyType::Bool);
        add(7, "StartTime",            FPropertyType::Int);     // int32 in stock UE5
        add(8, "UniqueId",             FPropertyType::Struct);  // FUniqueNetIdRepl (OnRep_UniqueId)
        add(9, "PlayerNamePrivate",    FPropertyType::String);  // FString (OnRep_PlayerName)
    }
    return c;
}

const ClassCatalog& aaoc_player_state_catalog() {
    static ClassCatalog c;
    c.class_name = "AAoCPlayerState";
    c.parent = &aplayer_state_catalog();
    return c;
}

const ClassCatalog& apawn_catalog() {
    static ClassCatalog c;
    c.class_name = "APawn";
    c.parent = &aactor_catalog();

    // Cluster at 0x14AA0B990..0x14AA0BAE0.  4 replicated properties.
    if (c.own_props.empty()) {
        auto add = [&](uint32_t rep_idx, const char* name,
                        FPropertyType t) {
            ReplicatedPropertyDesc d;
            d.rep_index = rep_idx;
            d.name = name;
            d.type = t;
            c.own_props.push_back(std::move(d));
        };
        //     RepIdx  Name                Type
        //     ------  ----------------    ----------------------------
        add(0, "RemoteViewPitch16",  FPropertyType::Int);     // uint16 fixed-precision
        add(1, "RemoteViewPitch",    FPropertyType::Byte);    // uint8 fixed-precision
        add(2, "PlayerState",        FPropertyType::Object);  // APlayerState* (OnRep_PlayerState)
        add(3, "Controller",         FPropertyType::Object);  // AController*  (OnRep_Controller)
    }
    return c;
}

const ClassCatalog& acharacter_catalog() {
    static ClassCatalog c;
    c.class_name = "ACharacter";
    c.parent = &apawn_catalog();
    return c;
}

const ClassCatalog& aaoc_pawn_catalog() {
    static ClassCatalog c;
    c.class_name = "AAoCPlayerPawn";   // placeholder; rename when IDA confirms
    c.parent = &acharacter_catalog();
    return c;
}

}}}} // namespace aoc::protocol::emit::replayout
