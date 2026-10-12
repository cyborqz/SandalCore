// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/player_pawn_emitter.cpp  —  PM45 (2026-04-30) — Path Y native player Pawn
//
//  Replaces the PM42 splice path which was abandoned because captured pkts
//  89-93 are session-bound transactions that leak captured-session state
//  (NetGUIDs 10341538, 10341548, etc.) into our session.  See PM44 cleanup
//  comment in world_bootstrap_plan.cpp for full diagnosis.
//
//  This native emitter mirrors PcEmitter's proven-clean ActorBuilder path:
//    - Mints actor NetGUID from NetGUIDAllocator (block.player_pawn)
//    - Uses content-addressable class CDO NetGUIDs (same hash on every server)
//    - Emits ActorOpen + SerializeNewActor + 8 subobject SerializeNewActor
//      blocks for the player Pawn's replicated components
//    - Strips RepLayout property tail for Phase B (defaults from CDO)
//    - Phase D will add per-property baselines once descriptor table is RE'd
//
//  ── Confirmed from RE + verbose log ────────────────────────────────────
//    Class path:  /Game/ThirdPersonCPP/Blueprints/PlayerPawn.Default__PlayerPawn_C
//    Captured class CDO NetGUID: 9327572450991073355 (content-addressable)
//    Instance size: 13,600 B (sub_145DEF450's a5 param)
//    Parent class: AIntrepidCharacter (sub_145948AC0 superclass getter)
//    8 replicated subobjects (per verbose log of working launch_all.bat):
//      - BaseCharacterInfo:     PlayerInfo
//      - CombatInfo:            PlayerCombatInfo
//      - OwnerInfo:             OwnerInfoComponent
//      - BackpackComponent:     ItemStorageComponent
//      - EquipmentComponent:    ItemStorageComponent
//      - QuestStorageComponent: ItemStorageComponent
//      - RewardStorageComponent:RewardStorageComponent
//      - Character Appearance:  CharacterAppearanceComponent (BP-added)
//
//  ── Channel + chSeq strategy ──────────────────────────────────────────
//    Channel: 19 (matches captured for routing convention; fresh in our session)
//    ChSeq:   let send_bunch_packet's natural chSeq accounting handle it.
//             We do NOT hardcode 954 (the captured value) — that's the trap
//             PM43 fell into.  Our session's ch=19 InReliable starts fresh.
// ============================================================================
#include "net/player_pawn_emitter.h"
#include "net/appearance_data.h"                    // PD2.1 — appearance payload builder
#include "net/native_connect_sequencer.h"           // IGameServerHost

#include "protocol/emit/actor_builder.h"
#include "protocol/emit/actor_bunch.h"
#include "protocol/emit/bunch_writer.h"
#include "protocol/emit/intrepid_netguid.h"
#include "protocol/emit/package_map_exporter.h"
#include "protocol/emit/rep_layout_writer.h"
#include "protocol/emit/schema_value.h"
#include "protocol/schema/actor_rep_handles.h"
#include "protocol/schema/actor_schema.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

#include <spdlog/spdlog.h>
#include <memory>
#include <cstdlib>   // PM107: atof, atoi
#include <cmath>     // PM107: std::abs

namespace aoc { namespace net {

using namespace aoc::protocol;
// ~ C-Y-B - C.O.A ~

// ── Player Pawn schema (inline — local to this emitter) ─────────────────
//
// Keeping the schema local avoids touching schema_registry.cpp's load_all()
// which would conflict with the existing build_pawn_schema() (NPC variant).
// If/when we need this schema accessible to other emitters we can promote
// it to SchemaRegistry.
static schema::ActorSchema build_player_pawn_schema_inline() {
    schema::ActorSchema s;
    s.type = schema::ActorType::Pawn;
    s.class_name = "APlayerCharacter";
    s.default_blueprint_path = "/Game/ThirdPersonCPP/Blueprints/PlayerPawn";
    // Content-addressable hash — same on every server/client.  Sourced
    // from verbose log: "InternalLoadObject loaded PlayerPawn_C ...
    // from NetGUID <ObjectId: 9327572450991073355 | ServerId: 0 | Randomizer: 0>"
    s.archetype_netguid = 9327572450991073355ULL;
    // Same level as PC (PersistentLevel of Verra_World_Master)
    s.level_netguid = 16442478405498561049ULL;

    // Phase B: no root property baselines — client uses CDO defaults
    // (location/rotation/velocity = 0; bIsDead=false; etc.)
    s.root_properties = {};

    // PM111 (2026-05-04) — Phase D Step 2.3 native subobject registration.
    //
    // class_path is the UE5 module path that the client uses to identify the
    // C++ class.  Confirmed via IDA RE of AOCClient.exe.asm + cross-checked
    // against AssetRegistry.json (FModel export).  Module = `GameSystemsPlugin`
    // (Intrepid's internal systems plugin compiled into the client binary).
    //
    // class_netguid is left = 0 here; PlayerPawnEmitter::emit_open populates
    // it from a deterministic hash of the path string (so server and client
    // arrive at the same NetGUID without coordination).
    auto make_comp = [](const std::string& name,
                        const std::string& cls,
                        const std::string& class_path) {
        schema::ComponentSchema c;
        c.class_name = cls;
        c.class_path = class_path;
        c.default_blueprint_path = "";
        c.properties = {};                // Phase 1: defaults from CDO
        (void)name;
        return c;
    };

    // 8 replicated subobjects in the order verbose log emitted them.
    // Three of them share `ItemStorageComponent` (Backpack, Equipment, Quest).
    s.components = {
        make_comp("BaseCharacterInfo",      "PlayerInfo",
                  "/Script/GameSystemsPlugin.PlayerInfo"),
        make_comp("CombatInfo",             "PlayerCombatInfo",
                  "/Script/GameSystemsPlugin.PlayerCombatInfo"),
        make_comp("OwnerInfo",              "OwnerInfoComponent",
                  "/Script/GameSystemsPlugin.OwnerInfoComponent"),
        make_comp("BackpackComponent",      "ItemStorageComponent",
                  "/Script/GameSystemsPlugin.ItemStorageComponent"),
        make_comp("EquipmentComponent",     "ItemStorageComponent",
                  "/Script/GameSystemsPlugin.ItemStorageComponent"),
        make_comp("QuestStorageComponent",  "ItemStorageComponent",
                  "/Script/GameSystemsPlugin.ItemStorageComponent"),
        make_comp("RewardStorageComponent", "RewardStorageComponent",
                  "/Script/GameSystemsPlugin.RewardStorageComponent"),
        make_comp("Character Appearance",   "CharacterAppearanceComponent",
                  "/Script/GameSystemsPlugin.CharacterAppearanceComponent"),
    };

    return s;
}

// PM111 (2026-05-04) — deterministic content-addressable NetGUID for a class
// path.  Same hash on every server/client (the value is arbitrary as long
// as we EXPORT it via package_map_exports — the client builds its own
// `NetGUID -> path` map from what we send).  We use the same SplitMix64
// primitive that NetGUIDAllocator uses for randomizer derivation.
static uint64_t hash_class_path(const std::string& path) {
    uint64_t h = 0xCBF29CE484222325ULL;  // FNV-1a offset basis (stable seed)
    for (unsigned char c : path) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001B3ULL;            // FNV-1a prime
    }
    // Mix once more so similar prefixes don't collide on low bits
    h ^= (h >> 33);
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= (h >> 33);
    // PM142 (2026-05-07) — produce a SMALL odd ObjectId.
    //
    // Live AOC pcap analysis (ranger respawn pkt#122 Block 9) shows class
    // GUIDs with tiny ObjectIds like 17 (= `0x11`).  Our PM140 hash output
    // (~64 random bits) encoded to ~10 SIP bytes which the live wire never
    // produces — and seems to make AOC's PackageMap silently reject the
    // class load even when the path is registered.
    //
    // We build a small ObjectId by taking the hash mod (2^14) then shifting
    // up 1 + 1 to set bit 0 (Static) and avoid low system GUIDs (0..15).
    // Result: ObjectId in [17, 32785], odd, small SIP encoding (≤2 bytes).
    // Stable per class_path (FNV deterministic).
    //
    // GUID space: dynamic runtime block uses ObjectId 16777218+ (= 2^24+).
    // Our small Static IDs are in the low range, distinct from runtime
    // dynamic IDs (different parity: dynamic is even, static is odd).
    h &= 0x3FFFULL;                      // 14 bits of entropy
    h <<= 1;                             // shift up so bit 0 free
    h |= 1ULL;                           // bit 0 = Static
    h |= 0x10ULL;                        // ensure > 16 (avoid system GUIDs)
    return h;
}

// ── Helper: build N-level export entry (mirrors PcEmitter helpers) ──────
static emit::ExportEntry build_two_level(uint64_t leaf_obj,
                                          const std::string& leaf_path,
                                          uint32_t leaf_checksum,
                                          uint64_t outer_obj,
                                          const std::string& outer_path,
                                          uint32_t outer_checksum,
                                          bool no_load = false) {
    emit::ExportEntry leaf = emit::ExportEntry::asset(
        leaf_obj, leaf_path, leaf_checksum, no_load);
    emit::ExportEntry outer = emit::ExportEntry::asset(
        outer_obj, outer_path, outer_checksum, no_load);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(outer));
    return leaf;
}

static emit::ExportEntry build_three_level(uint64_t leaf_obj,
                                            const std::string& leaf_path,
                                            uint32_t leaf_checksum,
                                            uint64_t mid_obj,
                                            const std::string& mid_path,
                                            uint32_t mid_checksum,
                                            uint64_t outermost_obj,
                                            const std::string& outermost_path,
                                            uint32_t outermost_checksum,
                                            bool no_load = true) {
    emit::ExportEntry outermost = emit::ExportEntry::asset(
        outermost_obj, outermost_path, outermost_checksum, no_load);
    emit::ExportEntry mid = emit::ExportEntry::asset(
        mid_obj, mid_path, mid_checksum, no_load);
    mid.outer = std::make_unique<emit::ExportEntry>(std::move(outermost));
    emit::ExportEntry leaf = emit::ExportEntry::asset(
        leaf_obj, leaf_path, leaf_checksum, no_load);
    leaf.outer = std::make_unique<emit::ExportEntry>(std::move(mid));
    return leaf;
}

PlayerPawnEmitter::PlayerPawnEmitter(IGameServerHost& host,
                                       const std::string& client_key)
    : host_(host), client_key_(client_key) {}

bool PlayerPawnEmitter::emit_open(const sockaddr_in& client_addr) {
    spdlog::warn("[PlayerPawnEmitter] emit_open — building player Pawn ActorOpen");

    // ── Schema ──────────────────────────────────────────────────────────
    auto pawn_schema = build_player_pawn_schema_inline();

    // ── Export entries: PlayerPawn class + level + (no GMCommands for Pawn) ──
    //
    // The captured pkt 89's bExports section contains 2-3 entries.  Mirror
    // PcEmitter's pattern: leaf class + outer package, then PersistentLevel.
    //
    // NetGUIDs for the PlayerPawn class are content-addressable — captured
    // value 9327572450991073355 is reused.  The outer (the BP asset path)
    // we mint a placeholder; the client will register it on first sight.
    std::vector<emit::ExportEntry> exports;
// Cyb - C | O | A

    // [0] Default__PlayerPawn_C → its outer /Game/ThirdPersonCPP/Blueprints/PlayerPawn
    //
    // PM46 (2026-04-30) — fix CNSF caused by outer_obj=0:
    //   Client's PackageMap parser rejects NetGUID=0 as outer (treats as
    //   "null parent"), causing parser cursor desync → CNSF on the next
    //   bunch field read.  Use a deterministic non-zero hash placeholder.
    //   PcEmitter's PC outer is 4074085207143396457 (hash of
    //   /Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP).  Ours is
    //   the SIBLING path /Game/ThirdPersonCPP/Blueprints/PlayerPawn, so
    //   we use a related-but-distinct value.  The client's PackageMap
    //   binds NetGUID→path on first sight and reuses it for the session.
    {
        auto e = build_two_level(
            /*leaf_obj=*/       9327572450991073355ULL,     // captured class CDO
            /*leaf_path=*/      "Default__PlayerPawn_C",
            /*leaf_checksum=*/  0x00000000,                   // unknown; 0 = client computes
            /*outer_obj=*/      4074085207143396458ULL,       // unique non-zero (PC outer + 1)
            /*outer_path=*/     "/Game/ThirdPersonCPP/Blueprints/PlayerPawn",
            /*outer_checksum=*/ 0x00000000,
            /*no_load=*/        false);
        exports.push_back(std::move(e));
    }

    // [1] PersistentLevel (3-level chain — same as PcEmitter)
    {
        auto e = build_three_level(
            /*leaf_obj=*/        16442478405498561049ULL,
            /*leaf_path=*/       "PersistentLevel",
            /*leaf_checksum=*/   0x00000000,
            /*mid_obj=*/         16604466839667550161ULL,
            /*mid_path=*/        "Verra_World_Master",
            /*mid_checksum=*/    0x00000000,
            /*outermost_obj=*/   12923414834320654503ULL,
            /*outermost_path=*/  "/Game/Levels/Verra_World_Master/Verra_World_Master",
            /*outermost_checksum=*/ 0x00000000,
            /*no_load=*/         true);
        exports.push_back(std::move(e));
    }

    // ── PM111/PM113 (2026-05-04) — Native subobject class registrations ───
    //
    // Each of the 8 PlayerPawn components needs its CLASS to be registered
    // in the client's PackageMap before V3 subobject content blocks can
    // resolve them.  6 unique class paths (Backpack/Equipment/Quest share
    // ItemStorageComponent).
    //
    // PM113 (2026-05-04) — GATED on probe_class_exports.txt.  The 6 class
    // exports add ~5400 bits to the bunch, which pushes our 3011-bit
    // baseline Pawn ActorOpen past the 8192-bit MAX_PKT_BITS cap.  When the
    // BunchDataBits SerializeInt(MAX=8192) overflows it silently encodes
    // wrong → client parse fails → loading screen loop with no possession.
    //
    // probe_class_exports.txt = "0" or absent → exports OFF (PM107 baseline)
    // probe_class_exports.txt = "1"           → exports ON (need partial bunch)
    //
    // Always populate comp.class_netguid (deterministic hash of path) — that's
    // free.  Only the bytes-on-wire export is gated.
    // PM134 (2026-05-07) — use the SAME GameSystemsPlugin NetGUID that
    // pc_emitter.cpp uses for GlobalGMCommands export (and that matches
    // what the captured replay registers when the PC channel opens).
    // Empirically verified in client log line 4188:
    //   "RegisterNetGUIDFromPath_Client: NetGUID: ObjectId: 158953490572197689,
    //    PathName: /Script/GameSystemsPlugin"
    // PRIOR (broken): 0xA0C5C8E12345CDEF (11584886485765443055) — fabricated
    // value that didn't match the package's already-registered GUID, so
    // InternalLoadObject couldn't resolve the CharacterAppearanceComponent
    // class CDO under our outer chain → load returns NULL → V3 content block
    // payload parse fails → Bunch.IsError on the appearance bunch.
    static constexpr uint64_t kGameSystemsPluginPackageGuid =
        158953490572197689ULL;  // 0x234D1A6F08D8B639
    bool emit_class_exports = false;
    {
        std::FILE* fp = std::fopen("probe_class_exports.txt", "r");
        if (fp) {
            int v = 0;
            std::fscanf(fp, "%d", &v);
            std::fclose(fp);
            emit_class_exports = (v != 0);
        }
    }
    // PM133 (2026-05-07) — surgical export: ONLY the appearance class,
    // gated separately so we can enable it without busting the 8192-bit
    // bunch cap.  Adding all 6 unique component classes (~5400 bits) to
    // the Pawn ActorOpen overflows the cap; one class (~840 bits) fits.
    bool emit_appearance_class_only = false;
    {
        std::FILE* fp = std::fopen("probe_appearance_class_export.txt", "r");
        if (fp) {
            int v = 0;
            std::fscanf(fp, "%d", &v);
            std::fclose(fp);
            emit_appearance_class_only = (v != 0);
        }
    }
    {
        std::vector<std::string> already_exported;
        for (auto& comp : pawn_schema.components) {
            if (comp.class_path.empty()) continue;
            comp.class_netguid = hash_class_path(comp.class_path);

            // Three modes:
            //   probe_class_exports.txt=1                     → export ALL 6 unique classes (overflows cap)
            //   probe_appearance_class_export.txt=1           → export ONLY CharacterAppearanceComponent (PM133)
            //   neither                                        → no class exports (PM107 baseline)
            bool should_export = false;
            if (emit_class_exports) {
                should_export = true;
            } else if (emit_appearance_class_only &&
                       comp.class_name == "CharacterAppearanceComponent") {
                should_export = true;
            }
            if (!should_export) continue;

            bool seen = false;
            for (const auto& s : already_exported) {
                if (s == comp.class_path) { seen = true; break; }
            }
            if (seen) continue;
            already_exported.push_back(comp.class_path);
// [C-Y-B x CoA]

            // PM143 (2026-05-07) — set non-zero leaf_checksum so the export
            // wire carries `bHasNetworkChecksum=1`.
            //
            // PM143-A test (build 21:38) PROVED Hypothesis A:
            //   With checksum=0 the client's StaticFindObject returned NULL
            //   (line 4277: "has network checksum 0", no "loaded Class").
            //   With checksum=0xCC1A4D5E placeholder, the client DID load the
            //   class (line 3818: "has network checksum 3424275806") — but
            //   then rejected our value via NetChecksumMismatch (line 3820)
            //   with the local value LEAKED to us as 2253250103 = 0x864DDE37.
            //
            // PM143-B (build 21:??) — use the REAL NetCompatibleChecksum that
            // the client logged.  Per-class lookup table; expand as needed
            // when other components also use dynamic-creation.
            auto checksum_for = [](const std::string& class_name) -> uint32_t {
                // Real NetCompatibleChecksums (RE'd from client error log).
                if (class_name == "CharacterAppearanceComponent") return 0x864DDE37u; // 2253250103
                // Fallback placeholder for classes we haven't seen yet — will
                // trigger the same NetChecksumMismatch leak so we can capture
                // the real value, then add it above.
                return 0xCC1A4D5Eu;
            };
            const uint32_t leaf_checksum = checksum_for(comp.class_name);

            auto e = build_two_level(
                /*leaf_obj=*/       comp.class_netguid,
                /*leaf_path=*/      comp.class_name,
                /*leaf_checksum=*/  leaf_checksum,
                /*outer_obj=*/      kGameSystemsPluginPackageGuid,
                /*outer_path=*/     "/Script/GameSystemsPlugin",
                /*outer_checksum=*/ 0x00000000,
                /*no_load=*/        false);
            exports.push_back(std::move(e));

            spdlog::info("[PlayerPawnEmitter] PM111/PM133/PM143 exporting component "
                         "class '{}' netguid={} (path={}) checksum=0x{:08x}",
                         comp.class_name, comp.class_netguid, comp.class_path,
                         leaf_checksum);
        }
        if (!emit_class_exports && !emit_appearance_class_only) {
            spdlog::info("[PlayerPawnEmitter] PM113/PM133 class exports DISABLED "
                         "(probe_class_exports.txt and probe_appearance_class_export.txt "
                         "both absent or \"0\") — Pawn ActorOpen stays minimal");
        }
    }

    // ── PM116 (2026-05-04) — Subobject NetGUID pre-registration ────────────
    //
    // RE finding: AOC's `sub_143F2C340` (ReadContentBlockHeader) ALWAYS
    // calls UPackageMapClient::SerializeObject (vtable+832) to resolve
    // the subobject FIntrepidNetGUID against the PackageMap.  When the
    // lookup returns nullptr AND UNetConnection+0x240 bit 0 (AOC custom
    // mode flag) is NOT set, the parser fires ContentBlockHeaderObjFail
    // → connection close.  Captured replay had the AOC flag set (so it
    // tolerated nullptr); our session does not.
    //
    // Fix: pre-register each of the 8 subobject NetGUIDs with an explicit
    // export entry BEFORE the V3 content blocks reference them.  The
    // outer is the Pawn's archetype CDO (NetGUID 9327572450991073355,
    // already in our exports list) — semantically the subobjects are
    // "components of the PlayerPawn class CDO" which is close enough
    // for the lookup to succeed (the actual instance binding happens
    // when the bunch's SerializeNewActor runs and constructs the Pawn).
    //
    // Each subobject export uses FIntrepidNetGUID(pawn_obj + ci + 1, 0, 0)
    // — ServerId and Randomizer must be 0 so the V3 content block's
    // sub_guid (which we'll write with the same triple) matches exactly
    // when AOC compares them as 128-bit values.
    //
    // Gated on `probe_subobject_exports.txt` (default 0 = baseline).
    bool emit_subobject_exports = false;
    {
        std::FILE* fp = std::fopen("probe_subobject_exports.txt", "r");
        if (fp) {
            int v = 0;
            std::fscanf(fp, "%d", &v);
            std::fclose(fp);
            emit_subobject_exports = (v != 0);
        }
    }
// built with Cyb and C O A

    // Mark all entries as having checksums (matches PcEmitter convention,
    // proven byte-clean for PC ActorOpen)
    for (auto& e : exports) {
        e.has_checksum = true;
        if (e.outer) {
            e.outer->has_checksum = true;
            if (e.outer->outer) e.outer->outer->has_checksum = true;
        }
    }

    // ── Mint actor + use deterministic Randomizer ──────────────────────
    auto block = host_.allocate_player_block(client_key_);
    auto rnd_for = [](uint64_t obj) -> uint32_t {
        // Same hash as bootstrap::NetGUIDAllocator::alloc_rnd_for —
        // deterministic per ObjectId so repeated encodes match.
        uint64_t h = obj * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 33);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= (h >> 33);
        return static_cast<uint32_t>(h);
    };
    const uint64_t pawn_obj = block.pawn;

    // PM116/PM117: append 8 compact subobject exports if enabled.
    //
    // PM116 used `build_two_level` which writes the FULL outer chain
    // (including the archetype's path) inside every subobject entry —
    // ~1089 bits per entry × 8 = 8712 bits exports alone, pushed the
    // bunch to 11723 bits (well over the 8192 cap).
    //
    // PM117 uses a COMPACT one-level form: leaf has its own path, but the
    // outer entry has has_path=false so only its 128-bit NetGUID + 8-bit
    // flags byte are written (the client looks up that NetGUID against
    // the archetype CDO export it just registered earlier in this same
    // bunch — InternalLoadObject's recursion terminates as soon as
    // has_path=false is read, so no full path repeat).
    //
    // Per entry size:
    //   [128b leaf FIntrepidNetGUID][8b flags=has_path=1]
    //   [128b outer FIntrepidNetGUID][8b flags=0 → terminate]
    //   [32b FString length][N×8b chars + 8b null]    ← leaf path
    //   = 128 + 8 + 128 + 8 + 32 + 8(N+1) bits ≈ 304 + 8N
    //
    // For our 8 components (avg name ~16 chars): ~432 bits per entry.
    // Total exports: 8 × 432 = ~3456 bits.  Plus PM107 baseline (3011) +
    // 8 V3 content blocks (~1112) = ~7579 bits — under the 8192 cap.
    if (emit_subobject_exports) {
        // PD2.2 (2026-05-05) — FIX: use the spawned PAWN INSTANCE as the outer,
        // not the class CDO.  Subobjects in UE5 are children of the actor
        // INSTANCE in the UObject hierarchy.  PM116/117 used `archetype_outer`
        // (the CDO 9327572450991073355) which made the client resolve to the
        // CDO's TEMPLATE component instead of our spawned actor's instance →
        // ContentBlockHeaderObjFail because the V3 content block's sub_guid
        // pointed at a template, not an instance.
        const uint64_t archetype_outer = pawn_obj;  // = our spawned Pawn instance NetGUID
        for (size_t ci = 0; ci < pawn_schema.components.size(); ++ci) {
            const auto& comp = pawn_schema.components[ci];
            const uint64_t sub_obj = pawn_obj + ci + 1;
            const std::string slot_name =
                (ci == 0) ? "BaseCharacterInfo" :
                (ci == 1) ? "CombatInfo" :
                (ci == 2) ? "OwnerInfo" :
                (ci == 3) ? "BackpackComponent" :
                (ci == 4) ? "EquipmentComponent" :
                (ci == 5) ? "QuestStorageComponent" :
                (ci == 6) ? "RewardStorageComponent" :
                            "Character Appearance";

            // PM117 compact form: leaf has path, outer is bare reference
            // (has_path=false → InternalLoadObject recursion terminates,
            // no path or further outer chain bytes are written).
            emit::ExportEntry leaf;
            leaf.guid.ObjectId   = sub_obj;
            leaf.guid.ServerId   = 0;
            leaf.guid.Randomizer = 0;
            leaf.has_path        = true;
            leaf.no_load         = false;
            leaf.has_checksum    = false;
            leaf.path            = slot_name;
// ~ C.O.A & C:Y:B ~

            // PM132 (2026-05-07) — outer_ref MUST use the FULL FIntrepidNetGUID
            // (ObjectId+ServerId+Randomizer) that matches what SerializeNewActor
            // registers for the Pawn.  Setting ServerId=0,Randomizer=0 made the
            // client treat this as a separate "phantom" outer: it registered the
            // path-based subobject under [pawn_obj|0|0] but the real Pawn was
            // registered later as [pawn_obj|60|rnd_for(pawn_obj)] — two
            // different NetGUID cache entries.  When AppearanceEmitter targets
            // a subobject of the Pawn, the lookup walks to outer [pawn_obj|0|0]
            // → finds nothing real → "Missing outer ... marking pending" →
            // OnRep_CharacterCustomization never fires → no visible mesh.
            //
            // Fix: ServerId=60 + Randomizer=rnd_for(pawn_obj) so outer_ref
            // matches the Pawn's eventual full NetGUID.  After SerializeNewActor
            // resolves the Pawn, the path-based subobjects link to it.
            emit::ExportEntry outer_ref;
            outer_ref.guid.ObjectId   = archetype_outer;
            outer_ref.guid.ServerId   = 60;
            outer_ref.guid.Randomizer = rnd_for(archetype_outer);
            outer_ref.has_path        = false;   // ← terminates recursion
            outer_ref.no_load         = false;
            outer_ref.has_checksum    = false;
            leaf.outer = std::make_unique<emit::ExportEntry>(std::move(outer_ref));

            exports.push_back(std::move(leaf));
            spdlog::info("[PlayerPawnEmitter] PM117 exporting subobject "
                         "'{}' (slot {}) netguid={} outer_ref={} ({})",
                         slot_name, ci, sub_obj, archetype_outer,
                         comp.class_name);
        }
    } else {
        spdlog::info("[PlayerPawnEmitter] PM116/117 subobject exports DISABLED "
                     "(probe_subobject_exports.txt absent or \"0\")");
    }

    emit::ActorRuntime rt;
    rt.type                 = schema::ActorType::Pawn;
    rt.actor_netguid        = pawn_obj;
    rt.actor_server_id      = 60;
    rt.actor_randomizer     = rnd_for(pawn_obj);
    rt.archetype_netguid    = 9327572450991073355ULL;
    rt.archetype_server_id  = 0;
    rt.archetype_randomizer = 0;
    rt.level_netguid        = 16442478405498561049ULL;
    rt.level_server_id      = 0;
    rt.level_randomizer     = 0;

    spdlog::warn("[PlayerPawnEmitter] Pawn NetGUID minted: ObjectId={} "
                  "ServerId={} Randomizer={} (block base=0x{:x}, "
                  "was captured=10341538)",
                  pawn_obj, rt.actor_server_id, rt.actor_randomizer, block.base);

    // PM104 (2026-05-04) — file-driven spawn coordinates.
    //
    // PRIOR (PM99): hardcoded Riverlands values (-7777622, 6166111, 159441).
    //   User tested → camera ended up too high; reported "we have been
    //   spawned in the skies".  Z = 159441 / 10 = 15944.1 cm = 159 m
    //   altitude was the captured replay's measured value, but Verra's
    //   actual terrain at that XY likely sits much lower than the captured
    //   PC's recorded position (the capture probably caught the PC mid-air
    //   on a hill, jumping, or before the engine settled it on ground).
    //
    // PM104: read x/y/z from probe_spawn_*.txt at runtime so we can iterate
    // without rebuilds.  Defaults stay at the Riverlands values; user can
    // edit dist/Release/probe_spawn_z.txt and restart server to retry.
    //
    // Useful values to try for Z (in scaled int = world_cm × 10):
    //     159441 = Riverlands captured (likely too high — current default)
    //     50000  = 5000 cm = 50m altitude
    //     10000  = 1000 cm = 10m altitude
    //         0  = sea level
    //   -10000  = below sea level (likely underground/ocean)
    //
    // Wire format: SerializePackedVector<10, 24> → 24-bit signed per axis.
    // Range: ±8,388,607 raw int, i.e. ±838,860.7 cm world per axis.
    // PM107 (2026-05-04) — accept BOTH scaled-int10 (e.g. "159441") AND
    // floating-point world-cm (e.g. "15944.113422") in the same probe file.
    //
    // PM104's reader used `%d` which silently truncates "15944.113422" → 15944,
    // then the encoder treats that as scaled int10 → world cm 1594.4 = 16m
    // altitude.  Player ends up underground.  This fix detects the decimal
    // point and converts to scaled int by `round(value * 10)`.
    //
    // Convention: if the file contains a '.' the value is treated as world cm
    // (and multiplied by the scale factor 10 to become scaled int10).  No '.'
    // → already scaled int10 (passed straight through).
    auto read_scaled_int10_file = [](const char* path, int32_t default_val) -> int32_t {
        std::FILE* fp = std::fopen(path, "r");
        if (!fp) return default_val;
        char buf[64] = {0};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
        std::fclose(fp);
        if (n == 0) return default_val;
        // Trim trailing whitespace/newlines
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'
                         || buf[n - 1] == ' '  || buf[n - 1] == '\t')) {
            buf[--n] = '\0';
        }
        // Detect floating-point form (any '.' or 'e'/'E')
        bool is_float = false;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { is_float = true; break; }
        }
        if (is_float) {
            double world_cm = std::atof(buf);
            // Round half-away-from-zero to scaled int10
            double scaled = world_cm * 10.0;
            if (scaled >= 0) return static_cast<int32_t>(scaled + 0.5);
            else             return static_cast<int32_t>(scaled - 0.5);
        }
        return std::atoi(buf);
    };
    const int32_t spawn_x = read_scaled_int10_file("probe_spawn_x.txt", -7777622);
    const int32_t spawn_y = read_scaled_int10_file("probe_spawn_y.txt",  6166111);
    const int32_t spawn_z = read_scaled_int10_file("probe_spawn_z.txt",   159441);
    spdlog::warn("[PlayerPawnEmitter] PM107 spawn (scaled int10): "
                  "x={} y={} z={} (world cm: x={:.1f} y={:.1f} z={:.1f})",
                  spawn_x, spawn_y, spawn_z,
                  spawn_x / 10.0, spawn_y / 10.0, spawn_z / 10.0);
    // Sanity check — Riverlands ground is around -77,776 cm world Y, +61,661 cm world Y.
    // If the user's resulting spawn is order-of-magnitude smaller (e.g. 1/10 of expected)
    // they probably wrote the cm value as plain int into the probe file (which expects
    // scaled int10 by default).  Warn loudly so they notice.
    if (std::abs(spawn_x) < 1'000'000 && std::abs(spawn_x) > 0) {
        spdlog::warn("[PlayerPawnEmitter] ★ spawn_x magnitude looks small "
                     "({}); did you mean {} (10x larger)?  "
                     "If the probe file has no decimal point it's read as scaled int10.",
                     spawn_x, spawn_x * 10);
    }

    rt.serialize_location   = true;
    rt.quantize_location    = true;
    rt.location_scaled_x    = spawn_x;
    rt.location_scaled_y    = spawn_y;
    rt.location_scaled_z    = spawn_z;
    rt.location_max_bits    = 24;
    // PM101 (2026-05-04) — REVERTED PM100.  Setting serialize_rotation=true
    // and serialize_velocity=true broke the Pawn ActorOpen because our
    // actor_builder encoder writes ONLY the bSerializeRotation/Velocity
    // header bits, NOT the actual rotation/velocity payload bits the client
    // expects to follow.
    //
    // Stock UE5 FRotator::SerializeCompressed (per axis):
    //   [1 bit bShortPitch]
    //   if (bShort) [16 bits Pitch]
    //   else        [8 bits Pitch]
    // → 27 bits total for all-zero rotation (3×9 bits)
    //
    // Our encoder (actor_builder.cpp lines 285-293) writes:
    //   [1 bit "is non-zero"]
    //   if (non-zero) [16 bits]
    // → 3 bits total for all-zero rotation
    //
    // Off by 24 bits → all subsequent bunch reads misalign →
    // ReadContentBlockHeader IsError on the NEXT bunch → connection close.
    //
    // To enable rotation/velocity replication later, the actor_builder
    // encoder needs to be fixed to match UE5's actual SerializeCompressed
    // format (1 bit bShort + 8 OR 16 bits value per axis, ALWAYS).
    // Phase D Step 2.1 — separate task; flagged in aoc_re_reference.md.
    rt.serialize_rotation   = false;
    rt.serialize_scale      = false;
    rt.serialize_velocity   = false;
// -- C | Y | B . C O A --

    // ── Bunch context ──────────────────────────────────────────────────
    emit::BunchWriter bw;
    emit::EmitContext ctx;
    ctx.channel              = 19;        // captured used ch=19; fresh in our session
    // PM51 (2026-04-30) — first chSeq on a fresh channel must equal
    //   InitInReliable + 1 = 953 + 1 = 954
    // The AOC client initializes any newly-opened channel's InReliable to
    // the connection-level InitInReliable (953 = 1977 & 0x3FF in our session)
    // and expects the first reliable bunch with chSeq = 954.  Sending 0
    // makes MakeRelative wrap to 1024 (= 953 + 71) → "Queuing bunch with
    // unreceived dependency: 1024 / 954" → channel never assembles.
    // PcEmitter hardcodes 954 for the same reason; we missed it for Pawn.
    ctx.ch_sequence          = 954;       // first chSeq on fresh ch=19
    ctx.is_reliable          = true;
    ctx.is_partial           = false;     // single-fragment if it fits
    ctx.partial_initial      = true;
    ctx.partial_final        = true;
    ctx.is_control           = true;      // channel-open control bunch
    ctx.b_open               = true;
    ctx.package_map_exports  = std::move(exports);
    ctx.ch_name_is_hardcoded = true;
    ctx.ch_name_ename_idx    = 102;       // NAME_Actor

    // ── Actor properties: roles, owner, controller ─────────────────────
    //
    // The original server's PlayerPawn_C carried Owner = PC and Role =
    // Authority in its open (replay pkt#78) and, once possessed (pkt#107),
    // RemoteRole = AutonomousProxy, Instigator = itself and Controller = PC.
    // Our pawn is possessed before it spawns, so all of it goes into the
    // open.  Without RemoteRole the client keeps the pawn a SimulatedProxy:
    // the camera follows it but it never moves.
    //
    // The block goes in as the spawn's tail, which replaces ActorBuilder's
    // legacy per-component blocks (and the probe-gated appearance payload).
    emit::FIntrepidNetworkGUID pawn_guid;
    pawn_guid.ObjectId   = pawn_obj;
    pawn_guid.ServerId   = 60;
    pawn_guid.Randomizer = rnd_for(pawn_obj);
    emit::FIntrepidNetworkGUID pc_guid;
    pc_guid.ObjectId   = block.player_controller;
    pc_guid.ServerId   = 60;
    pc_guid.Randomizer = rnd_for(block.player_controller);

    emit::BunchWriter props_bits;
    {
        using schema::ENetRole;
        emit::RepLayoutWriter props(props_bits);
        props.write_uint32(schema::actor_rep::AuthServerIDReplicated, 0);
        props.write_enum(schema::actor_rep::RemoteRole,
                         static_cast<uint32_t>(ENetRole::AutonomousProxy), schema::kNetRoleBits);
        props.write_object(schema::actor_rep::Owner, pc_guid);
        props.write_enum(schema::actor_rep::Role,
                         static_cast<uint32_t>(ENetRole::Authority), schema::kNetRoleBits);
        props.write_object(schema::actor_rep::Instigator, pawn_guid);
        props.write_object(schema::pawn_rep::Controller, pc_guid);
        props.finish();
    }
    emit::BunchWriter actor_block;
    emit::write_actor_property_block(actor_block, props_bits);
    ctx.spliced_tail_bits      = actor_block.data();
    ctx.spliced_tail_bit_count = actor_block.bit_pos();
    ctx.spliced_tail_start_bit = 0;
    spdlog::info("[PlayerPawnEmitter] actor properties: RemoteRole=AutonomousProxy, "
                 "Owner/Controller=PC {}, {} bits", pc_guid.ObjectId, actor_block.bit_pos());
// C Y B.CoA

    // ── Phase D Step 2.1 (2026-05-05) — Appearance payload ──────────────
    //
    // When `probe_appearance_emit.txt` == "1", we build the property-update
    // payload for the CharacterAppearanceComponent (8th subobject, ci=7) and
    // pass it to ActorBuilder via `ctx.appearance_payload_*`.  The builder
    // splices it into the V3 content block for that subobject — the client's
    // OnRep_CharacterCustomization fires on bunch processing → mesh assembles.
    //
    // First-iteration values (defaults; iterate via probe files):
    //   probe_appearance_handle_custom.txt    (default 0) — CharacterCustomization
    //   probe_appearance_handle_force_hide.txt(default 1) — bForceHideHeldItems
    //   probe_max.txt                          (default 4) — class NetCache size
    //
    // Once mesh appears with empty defaults, wire up real customization JSON
    // from data/characters.json (xclient_service stored it).
    std::vector<uint8_t> appearance_bits_buffer;
    uint32_t appearance_bits_count = 0;
    bool appearance_enabled = false;
    {
        // PD2.1 uses its own probe to avoid colliding with the legacy
        // AppearanceEmitter (which fires a SEPARATE bunch on ch=19 chSeq=955
        // that PM108 found breaks possession).  Keep that emitter's
        // probe_appearance_emit.txt at "0".
        std::FILE* fp = std::fopen("probe_pd2_appearance.txt", "r");
        if (fp) {
            int v = 0;
            std::fscanf(fp, "%d", &v);
            std::fclose(fp);
            appearance_enabled = (v != 0);
        }
    }
    if (appearance_enabled) {
        auto read_uint = [](const char* path, uint32_t default_val) -> uint32_t {
            std::FILE* f = std::fopen(path, "r");
            if (!f) return default_val;
            uint32_t n = default_val;
            std::fscanf(f, "%u", &n);
            std::fclose(f);
            return n;
        };
        // Use a DEDICATED probe for appearance MAX, so iterating doesn't
        // interfere with PC's probe_max.txt (which is hardcoded at 1024).
        // Default 4: small NetCache typical for a 2-property component.
        const uint32_t handle_max = read_uint("probe_appearance_max.txt", 4u);
        const uint32_t handle_custom    = read_uint("probe_appearance_handle_custom.txt",     0u);
        const uint32_t handle_force_hide = read_uint("probe_appearance_handle_force_hide.txt", 1u);

        aoc::net::CharacterCustomizationData cc;
        // Iteration 0: use defaults (initial test — does the format parse?).
        // Once it does, swap with parse_customization_json(json_for_selected_char, cc)
        // sourced from xclient_service's stored character data.
        cc.skin_color_hue          = 0.5f;
        cc.skin_color_pigmentation = 0.5f;
        cc.head_hair_length        = 0.5f;
        cc.is_helmet_visible       = true;
        cc.is_cape_visible         = true;
        cc.force_hide_held_items   = false;

        appearance_bits_count = aoc::net::build_appearance_payload_bits(
            cc,
            handle_max,
            handle_custom,
            handle_force_hide,
            appearance_bits_buffer);

        ctx.appearance_payload_bits      = appearance_bits_buffer.empty()
                                              ? nullptr
                                              : appearance_bits_buffer.data();
        ctx.appearance_payload_bit_count = appearance_bits_count;
        ctx.appearance_subobject_index   = 7;  // CharacterAppearance is 8th comp
        // Force V3 wrap even if payload is 0 bits (mode 0 = empty content
        // block; client falls back to local lobby character data).
        ctx.appearance_force_v3_wrap     = true;

        spdlog::warn("[PlayerPawnEmitter] PD2.1 appearance payload ENABLED: "
                      "{} bits ({} bytes), max={} h_custom={} h_force_hide={} v3_wrap=true",
                      appearance_bits_count, appearance_bits_buffer.size(),
                      handle_max, handle_custom, handle_force_hide);
    } else {
        spdlog::info("[PlayerPawnEmitter] PD2.1 appearance payload DISABLED "
                     "(probe_appearance_emit.txt absent or \"0\")");
    }

    // ── Build bunch ────────────────────────────────────────────────────
    emit::ActorBuilder builder;
    size_t bits = builder.build_spawn(pawn_schema, rt, ctx, bw);
    if (bits == 0) {
        spdlog::error("[PlayerPawnEmitter] ActorBuilder::build_spawn returned 0 bits");
        return false;
    }
    spdlog::info("[PlayerPawnEmitter] ActorBuilder produced {} bits ({} bytes) "
                 "for {} subobjects",
                 bits, (bits + 7) / 8, pawn_schema.components.size());

    // ── Send ───────────────────────────────────────────────────────────
    bool ok = host_.send_bunch_packet(client_key_, client_addr, bw.data(), bits);
    if (!ok) {
        spdlog::error("[PlayerPawnEmitter] send_bunch_packet failed");
        return false;
    }

    spdlog::warn("[PlayerPawnEmitter] ★ Player Pawn ActorOpen sent "
                  "(PM45: native, ch=19, NetGUID {}, archetype=PlayerPawn_C)",
                  pawn_obj);
    return true;
}

bool PlayerPawnEmitter::emit_hidden(const sockaddr_in& client_addr, uint32_t channel,
                                    uint32_t ch_seq, bool hidden) {
    emit::BunchWriter props_bits;
    {
        emit::RepLayoutWriter props(props_bits);
        props.write_bool(schema::actor_rep::bHidden, hidden);
        props.finish();
    }
    emit::BunchWriter block;
    emit::write_actor_property_block(block, props_bits);
    emit::BunchWriter bw;
    emit::write_reliable_actor_bunch(bw, channel, ch_seq, /*has_exports=*/false,
                                     block.data(), 0, block.bit_pos());
    spdlog::warn("[PlayerPawnEmitter] bHidden={} (ch={} chSeq={})", hidden, channel, ch_seq);
    return host_.send_bunch_packet(client_key_, client_addr, bw.data(), bw.bit_pos());
}

}} // namespace aoc::net
