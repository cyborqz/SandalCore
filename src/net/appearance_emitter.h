// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/appearance_emitter.h
//
//  Phase D Step 2 (2026-05-04) — replicate Pawn appearance properties post-
//  possession so the player's MergeableSkeletalMeshComponent assembles a
//  visible body.
//
//  ── Why this is needed ──────────────────────────────────────────────────
//  Per FModel CDO dump of /Game/ThirdPersonCPP/Blueprints/PlayerPawn.uasset:
//
//    - PlayerPawn_C contains a `MergeableSkeletalMeshComponent` named
//      "CharacterMesh0" — AOC's custom mesh class that ASSEMBLES the final
//      mesh at runtime by merging head + body + armor + cosmetic parts.
//    - The CDO ships with NO default mesh; the merge result is empty until
//      `CharacterAppearanceComponent` provides data via property updates.
//    - The Nameplate component attaches to `CharacterMesh0:root` bone — with
//      no mesh loaded, the nameplate falls to actor origin (= what you see
//      floating: "Player" tag with no body).
//
//  ── Discovered via ASM grep of OnRep_* property names ───────────────────
//  Replicated properties on CharacterAppearanceComponent (via ASM strings):
//
//    - AppearanceIDs            (TArray, field offset 0x4D0)
//    - SharedAppearanceInfoId   (~16 bytes, field offset 0x4E0)
//    - CharacterCustomization   (struct, on parent component)
//    - CosmeticData             (struct)
//    - ForceHideHeldItems       (bool)
//
//  RepIndices are not yet known.  When we extract them (later), the
//  property update bunches plug into the wire builder unchanged.
//
//  ── Strategy ────────────────────────────────────────────────────────────
//  1. Fire AFTER possession (after PcEmitter::emit_pawn_link succeeds).
//  2. Build property update bunches on the Pawn channel (ch=19), targeting
//     the `Character Appearance` subobject by NetGUID.
//  3. Wire format reuses PropertyUpdateBunchBuilder's V3 + 10-bit handle +
//     no-end-marker pattern proven in PM97.
//
//  Phase D Step 2.0 (this file): emitter scaffolding only — sends nothing
//  yet, but proves the call path.  Phase D Step 2.1 will add real bytes.
//
//  LAYER:   net
//  OWNER:   Phase D Step 2
//  SESSION: 2026-05-04
// ============================================================================
#pragma once
// (C Y B) (CoA)

#include <cstdint>
#include <string>
#include <vector>

struct sockaddr_in;

namespace aoc { namespace net {

class IGameServerHost;

/// State / metadata about an appearance to seed.  Each field maps to one
/// replicated property on `UCharacterAppearanceComponent`.
///
/// All fields have safe defaults (empty / zero) — the emitter will skip
/// properties that have empty data unless the caller explicitly opts in.
/// Once we have the right binary layouts, the caller populates real values.
struct AppearanceSeed {
    /// Body / armor / cosmetic asset IDs.  Each ID is an AOC content
    /// reference (looked up against the cooked character data table).
    std::vector<uint32_t> appearance_ids;

    /// Shared appearance info — single struct (likely `FAppearanceInfo`)
    /// describing morph weights + race + gender.  Bytes are kept opaque
    /// here because we don't yet know the field layout.  Caller must
    /// match the on-wire byte count to the struct's RepIndex.
    std::vector<uint8_t> shared_appearance_info_bytes;

    /// Customization morph data — 16 floats per face/body slider per the
    /// captured replay analysis.  Empty array = skip the property.
    std::vector<float> character_customization;

    /// Optional: explicit SkeletalMesh asset NetGUID for direct mesh
    /// assignment (bypasses the merge system if AOC supports it).  Zero
    /// means "let the merge system pick from the CDO defaults."
    uint64_t override_skeletal_mesh_netguid = 0;

    /// Hide all held weapon visuals (useful while we're still figuring
    /// out the equipment system).
    bool force_hide_held_items = true;
};

/// Emitter that seeds the Pawn's CharacterAppearanceComponent with
/// property updates after possession completes.
///
/// Call sequence:
///   1. PlayerPawnEmitter::emit_open    — opens Pawn channel ch=19
///   2. PcEmitter::emit_pawn_link        — fires ClientRestart RPC
///   3. AppearanceEmitter::emit_seed     — sends property updates
class AppearanceEmitter {
public:
    AppearanceEmitter(IGameServerHost& host, const std::string& client_key);

    /// Send a sequence of property update bunches on the Pawn channel
    /// targeting the `Character Appearance` subobject.  Returns true if
    /// all bunches were dispatched without errors.
    ///
    /// The Pawn channel + appearance-component subobject NetGUIDs are
    /// derived from the player's allocated NetGUID block — same source
    /// PlayerPawnEmitter uses for the actor open.
    bool emit_seed(const sockaddr_in& client_addr, const AppearanceSeed& seed);

    /// Convenience — emits an "all-defaults" seed.  Use to demonstrate
    /// the property update pipeline works end-to-end before we have
    /// real appearance data.
    bool emit_default_seed(const sockaddr_in& client_addr) {
        AppearanceSeed s;  // empty / zero everything
        return emit_seed(client_addr, s);
    }

private:
    IGameServerHost& host_;
    std::string client_key_;
};

}} // namespace aoc::net
