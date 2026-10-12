// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/world_bootstrap_plan.cpp
//
//  The default Road A bootstrap plan — describes the first 100 captured
//  packets in terms of how WorldBootstrapEmitter should reproduce each.
//
//  ── Plan classification (per docs/native-bootstrap-sequence.md and
//     docs/BOOTSTRAP-STATIC-VS-DYNAMIC.md) ──
//
//  pkt #0       NativePc0      AoC opcode 3 — already handled by BootstrapEmitter
//  pkt #1       Skip           server-frame-time keepalive
//  pkt #2       Splice         NMT_Welcome re-emit (the live NMT handler
//                              already sent NMT_Welcome during handshake;
//                              we splice this re-emit just to match the
//                              captured stream's chSeq advancement)
//  pkts #3-#21  Skip           19 sentinel keepalives (21B bb=1 each)
//  pkt #22      NativePc22     PC ActorOpen — PcEmitter (with minted GUID)
//                              + initial Name property update
//  pkts #23-#77 Splice         PC continuation tail + ch=85 GUIDExports +
//                              ch=2 GUIDExport + various other actor opens
//  pkt #78      NativePawn78   Pawn ActorOpen — PawnEmitter (currently
//                              splices captured pkt#78 bytes; will be
//                              replaced with native ActorBuilder later)
//  pkts #79-#99 Splice         Initial PlayerState replication tick + NPC
//                              spawns + initial property values (HP/MP/etc.)
//
//  Pacing: 15 ms between most packets (matches captured cadence); 100 ms
//  before the PC ActorOpen so the client processes early GUIDExports first.
//
//  ── How to upgrade entries to native ──
//  1. Add a new EmissionMode for the packet (e.g. NativePcTail23)
//  2. Add a dispatch case in WorldBootstrapEmitter::dispatch_one
//  3. Flip this row's mode here
//  4. Test (byte-identity ideally; in-game otherwise)
// ============================================================================
#include "net/world_bootstrap_emitter.h"

// ── OPTION C — minimal plan for divergence-isolation test (2026-04-27) ──
//
// When 1, strips most post-PC-chain rows (47-77 and 79+) so the only
// captured packets emitted are: pkt#0 (opcode 3), pkt#2 (NMT_Welcome),
// pkt#22-46 (the full multi-fragment PC ActorOpen chain), pkt#78 (Pawn
// ActorOpen via PawnEmitter).  Goal: prove or disprove that the
// loading-screen loop is caused by post-spawn world replication
// divergence (captured session opens different streamed sublevels than
// our live session, since the captured player spawned in a different
// world tile).
//
// Expected outcomes:
//   * World renders empty + character visible at origin
//     → divergence is the blocker; next step is a native
//       ServerUpdateLevelVisibility responder (Option B).
//   * Still loading-screen loop with a different failure signature
//     → client expects a specific post-spawn property tick (HP, name,
//       team, etc.); add a single targeted Splice row back and retest.
//   * Disconnect shortly after pkt#78
//     → channel stream starvation; Maintain loop must keep
//       channels alive after Pawn open.
//
// Flip back to 0 to restore the full ~500-packet plan.
#ifndef AOC_OPTION_C_MINIMAL
#define AOC_OPTION_C_MINIMAL 1
#endif
// ~ C.O.A & Cyb ~

namespace aoc { namespace net {

const std::vector<PacketEmissionSpec> kDefaultBootstrapPlan = {
    // ── Phase 1: Post-NMT echoes ─────────────────────────────────────────
    // Pacing matches working hybrid baseline (~150ms avg per packet).
    {  0, EmissionMode::NativePc0, "AoC opcode 3 (BootstrapEmitter)",  20 },
    {  1, EmissionMode::Skip,      "frame-time keepalive",              5 },
    {  2, EmissionMode::Splice,    "NMT_Welcome re-emit",              30 },

    // ── Phase 2: Sentinel fillers (19 keepalives) ───────────────────────
    // The captured server emitted these while the client processed the
    // NMT_Welcome and built up to NMT_NetSpeed/NMT_GameSpecific.  Our
    // Maintain loop's natural cadence covers this — Skip them.
    {  3, EmissionMode::Skip, "filler #3",  5 }, {  4, EmissionMode::Skip, "filler #4",  5 },
    {  5, EmissionMode::Skip, "filler #5",  5 }, {  6, EmissionMode::Skip, "filler #6",  5 },
    {  7, EmissionMode::Skip, "filler #7",  5 }, {  8, EmissionMode::Skip, "filler #8",  5 },
    {  9, EmissionMode::Skip, "filler #9",  5 }, { 10, EmissionMode::Skip, "filler #10", 5 },
    { 11, EmissionMode::Skip, "filler #11", 5 }, { 12, EmissionMode::Skip, "filler #12", 5 },
    { 13, EmissionMode::Skip, "filler #13", 5 }, { 14, EmissionMode::Skip, "filler #14", 5 },
    { 15, EmissionMode::Skip, "filler #15", 5 }, { 16, EmissionMode::Skip, "filler #16", 5 },
    { 17, EmissionMode::Skip, "filler #17", 5 }, { 18, EmissionMode::Skip, "filler #18", 5 },
    { 19, EmissionMode::Skip, "filler #19", 5 }, { 20, EmissionMode::Skip, "filler #20", 5 },
    { 21, EmissionMode::Skip, "filler #21", 5 },

    // ── Phase 3: PC ActorOpen — Path B2 Step 1 (PM35, 2026-04-29) ──────
    //
    // CHANGE: pkts 22-46 are now SKIPPED.  We replace the entire captured
    // PC chain with a NATIVE emit (NativePc22).
    //
    // Why: verbose UE5 net log proved the captured PC chain establishes
    // session-specific NetGUIDs that don't exist in our session.  Captured
    // pkts 45/46 emit unreliable property updates referencing the captured
    // PC's subobjects (CombatInfo/AbilityComponent/StatsComponent w/ Outer
    // 599490) which fail to resolve in our session → "InternalLoadObject
    // loaded NULL" → property update reads garbage FString → CNSF.
    //
    // The earlier comment about chSeq mismatch (954 vs 1978) is still
    // relevant but in this iteration we strip the captured tail from
    // PcEmitter (no spliced 848 bits referencing dead NetGUIDs), and the
    // 1024-bunch gap concern is addressed separately by the chSeq tracker
    // already running across all reliable channels.
    //
    // STEP 1 SCOPE: just the ActorOpen — PC has DEFAULT initial property
    // values (no Pawn ref, no PlayerState ref).  Step 2 will add the
    // PC.Pawn property update so AcknowledgePossession can fire.
    { 22, EmissionMode::NativePc22, "PC ActorOpen (B2 native, no captured tail)", 100 },
    { 23, EmissionMode::Skip,       "PC continuation #1 (was splice)",            5 },

    // ── Phase 4: skip captured GUIDExport chain ─────────────────────────
    // These exported subobject NetGUIDs of the captured PC.  We're using
    // our own PC, so these would just confuse the client.
    { 24, EmissionMode::Skip, "B2-skip: ch=85 GUIDExport #1",          5 },
    { 25, EmissionMode::Skip, "B2-skip: ch=2 GUIDExport + ch=30 close",5 },
    { 26, EmissionMode::Skip, "B2-skip: ch=85 GUIDExport #2",          5 },
    { 27, EmissionMode::Skip, "B2-skip: PARSE_FAIL / opaque",          5 },

    // ── GameState (AoCGameStateBP_C, ch=6): its open is an 18-bunch partial
    // chain in captured pkts 28-45 (chSeq 954-971).  Until it arrives the
    // client world never begins play, so no actor ticks and the loading
    // screen keeps "Waiting for World Partition Streaming".  Only ch=6 is
    // re-sent; pkt 45 also carries an NPC (ch=7), which stays out.
    { 28, EmissionMode::SpliceChannel, "GameState open (ch=6) 1/18", 5, 6 },
    { 29, EmissionMode::SpliceChannel, "GameState open (ch=6) 2/18", 5, 6 },
    { 30, EmissionMode::SpliceChannel, "GameState open (ch=6) 3/18", 5, 6 },
    { 31, EmissionMode::SpliceChannel, "GameState open (ch=6) 4/18", 5, 6 },
    { 32, EmissionMode::SpliceChannel, "GameState open (ch=6) 5/18", 5, 6 },
    { 33, EmissionMode::SpliceChannel, "GameState open (ch=6) 6/18", 5, 6 },
    { 34, EmissionMode::SpliceChannel, "GameState open (ch=6) 7/18", 5, 6 },
    { 35, EmissionMode::SpliceChannel, "GameState open (ch=6) 8/18", 5, 6 },
    { 36, EmissionMode::SpliceChannel, "GameState open (ch=6) 9/18", 5, 6 },
    { 37, EmissionMode::SpliceChannel, "GameState open (ch=6) 10/18", 5, 6 },
    { 38, EmissionMode::SpliceChannel, "GameState open (ch=6) 11/18", 5, 6 },
    { 39, EmissionMode::SpliceChannel, "GameState open (ch=6) 12/18", 5, 6 },
    { 40, EmissionMode::SpliceChannel, "GameState open (ch=6) 13/18", 5, 6 },
    { 41, EmissionMode::SpliceChannel, "GameState open (ch=6) 14/18", 5, 6 },
    { 42, EmissionMode::SpliceChannel, "GameState open (ch=6) 15/18", 5, 6 },
    { 43, EmissionMode::SpliceChannel, "GameState open (ch=6) 16/18", 5, 6 },
    { 44, EmissionMode::SpliceChannel, "GameState open (ch=6) 17/18", 5, 6 },
    { 45, EmissionMode::SpliceChannel, "GameState open (ch=6) 18/18", 5, 6 },

    // ── Phase 6: skip the PARSE_FAIL packets (CNSF source) ──────────────
    { 46, EmissionMode::Skip, "B2-skip: PARSE_FAIL #46 (CNSF source)", 5 },

    // ── Phase 7: ch=4+ other actors (NPCs / environment) ────────────────
#if AOC_OPTION_C_MINIMAL
    // Option C strip — these are world actors that the captured player saw
    // but our session may not have streamed yet.  Splicing causes channel
    // namespace collisions and "Channel name serialization failed" warns.
    { 47, EmissionMode::Skip, "OPTC strip: ch=4 actor begin", 5 },
    { 48, EmissionMode::Skip, "OPTC strip: actor #48", 5 }, { 49, EmissionMode::Skip, "OPTC strip: actor #49", 5 },
    { 50, EmissionMode::Skip, "OPTC strip: actor #50", 5 }, { 51, EmissionMode::Skip, "OPTC strip: actor #51", 5 },
    { 52, EmissionMode::Skip, "OPTC strip: actor #52", 5 }, { 53, EmissionMode::Skip, "OPTC strip: actor #53", 5 },
    { 54, EmissionMode::Skip, "OPTC strip: actor #54", 5 }, { 55, EmissionMode::Skip, "OPTC strip: actor #55", 5 },
    { 56, EmissionMode::Skip, "OPTC strip: actor #56", 5 }, { 57, EmissionMode::Skip, "OPTC strip: actor #57", 5 },
    { 58, EmissionMode::Skip, "OPTC strip: actor #58", 5 }, { 59, EmissionMode::Skip, "OPTC strip: actor #59", 5 },
    { 60, EmissionMode::Skip, "OPTC strip: actor #60", 5 }, { 61, EmissionMode::Skip, "OPTC strip: actor #61", 5 },
    { 62, EmissionMode::Skip, "OPTC strip: actor #62", 5 }, { 63, EmissionMode::Skip, "OPTC strip: actor #63", 5 },
    { 64, EmissionMode::Skip, "OPTC strip: actor #64", 5 }, { 65, EmissionMode::Skip, "OPTC strip: actor #65", 5 },
    { 66, EmissionMode::Skip, "OPTC strip: actor #66", 5 }, { 67, EmissionMode::Skip, "OPTC strip: actor #67", 5 },
    { 68, EmissionMode::Skip, "OPTC strip: actor #68", 5 }, { 69, EmissionMode::Skip, "OPTC strip: actor #69", 5 },
    { 70, EmissionMode::Skip, "OPTC strip: actor #70", 5 }, { 71, EmissionMode::Skip, "OPTC strip: actor #71", 5 },
    { 72, EmissionMode::Skip, "OPTC strip: actor #72", 5 }, { 73, EmissionMode::Skip, "OPTC strip: actor #73", 5 },
    { 74, EmissionMode::Skip, "OPTC strip: actor #74", 5 }, { 75, EmissionMode::Skip, "OPTC strip: actor #75", 5 },
    { 76, EmissionMode::Skip, "OPTC strip: actor #76", 5 }, { 77, EmissionMode::Skip, "OPTC strip: actor #77", 5 },
#else
    { 47, EmissionMode::Splice, "ch=4 actor begin", 30 },
    { 48, EmissionMode::Splice, "actor #48", 30 }, { 49, EmissionMode::Splice, "actor #49", 30 },
    { 50, EmissionMode::Splice, "actor #50", 30 }, { 51, EmissionMode::Splice, "actor #51", 30 },
    { 52, EmissionMode::Splice, "actor #52", 30 }, { 53, EmissionMode::Splice, "actor #53", 30 },
    { 54, EmissionMode::Splice, "actor #54", 30 }, { 55, EmissionMode::Splice, "actor #55", 30 },
    { 56, EmissionMode::Splice, "actor #56", 30 }, { 57, EmissionMode::Splice, "actor #57", 30 },
    { 58, EmissionMode::Splice, "actor #58", 30 }, { 59, EmissionMode::Splice, "actor #59", 30 },
    { 60, EmissionMode::Splice, "actor #60", 30 }, { 61, EmissionMode::Splice, "actor #61", 30 },
    { 62, EmissionMode::Splice, "actor #62", 30 }, { 63, EmissionMode::Splice, "actor #63", 30 },
    { 64, EmissionMode::Splice, "actor #64", 30 }, { 65, EmissionMode::Splice, "actor #65", 30 },
    { 66, EmissionMode::Splice, "actor #66", 30 }, { 67, EmissionMode::Splice, "actor #67", 30 },
    { 68, EmissionMode::Splice, "actor #68", 30 }, { 69, EmissionMode::Splice, "actor #69", 30 },
    { 70, EmissionMode::Splice, "actor #70", 30 }, { 71, EmissionMode::Splice, "actor #71", 30 },
    { 72, EmissionMode::Splice, "actor #72", 30 }, { 73, EmissionMode::Splice, "actor #73", 30 },
    { 74, EmissionMode::Splice, "actor #74", 30 }, { 75, EmissionMode::Splice, "actor #75", 30 },
    { 76, EmissionMode::Splice, "actor #76", 30 }, { 77, EmissionMode::Splice, "actor #77", 30 },
#endif // AOC_OPTION_C_MINIMAL

    // pkt#78 — PM45 (2026-04-30) — NATIVE PLAYER PAWN.
    //
    // Path Y commits: PM43 splice abandoned (session-bound captured state
    // contamination), now emitting a fully-native player Pawn ActorOpen
    // via PlayerPawnEmitter.  Mirrors PcEmitter's proven-clean ActorBuilder
    // pattern.  Uses our session's minted NetGUID (block.player_pawn) for
    // the actor and content-addressable hashes for class CDOs.
    //
    // Subobjects (per verbose log of working launch_all.bat):
    //   BaseCharacterInfo:UPlayerInfo, CombatInfo:UPlayerCombatInfo,
    //   OwnerInfo:UOwnerInfoComponent, BackpackComponent/EquipmentComponent/
    //   QuestStorageComponent:UItemStorageComponent,
    //   RewardStorageComponent:URewardStorageComponent,
    //   "Character Appearance":UCharacterAppearanceComponent.
    //
    // Phase B scope: actor + 8 subobject channel opens with default
    // properties (CDO-resolved on client).  Phase C will link PC.Pawn to
    // this Pawn to fire AcknowledgePossession.  Phase D will add per-prop
    // baselines as the client demands them.
    { 78, EmissionMode::NativePlayerPawn,
        "PM45: native player Pawn ActorOpen (replaces PM43 splice attempt)", 5 },

    // ── PM37 (2026-04-29) — Path C Phase 1.5 / Option B: skip ALL captured world ──
    //
    // PM36 confirmed the chSeq renumber works: captured bunches now FLOW
    // (no longer queued).  But once flowing, the client tries to parse
    // them — and a property update on Node_Villager_F_C references
    // captured NetGUID 970 which doesn't exist in our session →
    // "InternalLoadObject loaded NULL" → ContentBlockFail → connection
    // killed ("Connection to the Realm timed out").
    //
    // The captured world data references THOUSANDS of NetGUIDs that
    // belong to the captured session.  We can't whack-a-mole every one.
    //
    // OPTION B: skip ALL captured packets after pkt 78.  Result:
    //   - PC ActorOpen (pkt 22 native via PcEmitter) ✓
    //   - Pawn ActorOpen-equivalent (pkt 78 via PawnEmitter — Guard NPC) ✓
    //   - NO captured world data after pkt 78 ✓
    //   - Empty world but stable connection
    //
    // Phase 2 (next) will add a native player Pawn emitter so we have
    // a controllable character.  Phase 3 will link PC.Pawn → our Pawn.
    //
    // Once we have a stable foundation, future phases can add native
    // NPCs / world content.  For now, stability > content.
// -- Cyb . C.O.A --

    // (Original comment kept for traceability — now superseded by Option B)
#if AOC_OPTION_C_MINIMAL
    // Bridge pkts 79-133 — PM37: now SKIP (was Splice).
    // Captured property updates reference dead NetGUIDs → ContentBlockFail.
    { 79, EmissionMode::Skip, "PM37 OptB: skip captured Pawn rep #79",   5 }, { 80, EmissionMode::Skip, "PM37 OptB: #80",   5 },
    { 81, EmissionMode::Skip, "PM37 OptB: #81",   5 }, { 82, EmissionMode::Skip, "PM37 OptB: #82",   5 },
    { 83, EmissionMode::Skip, "PM37 OptB: #83",   5 }, { 84, EmissionMode::Skip, "PM37 OptB: #84",   5 },
    { 85, EmissionMode::Skip, "PM37 OptB: #85",   5 }, { 86, EmissionMode::Skip, "PM37 OptB: #86",   5 },
    { 87, EmissionMode::Skip, "PM37 OptB: #87",   5 }, { 88, EmissionMode::Skip, "PM37 OptB: #88",   5 },
    // ── PM44 (2026-04-30) — PM43 splice ABANDONED, reverted to Skip ─────
    // PM43 attempted to splice the captured ch=19 player Pawn partial bunch
    // chain (pkts 89-93).  Live test (10:08) revealed the splice has the
    // SAME fundamental flaw as PM36: captured packets are session-bound.
    //
    //   pkt 89 wire: bunch[0] = ch=3  Seq 991 (captured PC tail leak)
    //                bunch[1] = ch=19 Seq 962 (Pawn open chunk 1)
    //   client log:  "Queuing bunch with unreceived dependency: 991/954"
    //                "Queuing bunch with unreceived dependency: 962/954"
    //                "InternalLoadObject loaded NULL: NetGUID 10341538"
    //                "InternalLoadObject loaded NULL: NetGUID 10341548"
    //                "InternalLoadObject loaded NULL: NetGUID 10341550"
    //
    // Root cause: a captured replay is a SESSION-BOUND transaction.  Its
    // bunches reference (a) chSeq state advanced by skipped earlier bunches,
    // (b) NetGUIDs minted by the captured server, (c) channel-index claims
    // that conflict with our own session.  Splicing fragments leaks all
    // three.  Legacy mode works because it replays the full closure.
    //
    // Path C Phase 2 will be NATIVE (PlayerPawnEmitter built from scratch
    // using ActorBuilder + a correct PlayerPawn_C schema).  Replacing
    // pawn_schema.cpp's NPC-shaped components with the verbose-log-confirmed
    // 8-component player layout is the next milestone.
    { 89, EmissionMode::Skip, "PM44: PM43 splice abandoned — see comment", 5 },
    { 90, EmissionMode::Skip, "PM44: PM43 splice abandoned",                5 },
    { 91, EmissionMode::Skip, "PM44: PM43 splice abandoned",                5 },
    { 92, EmissionMode::Skip, "PM44: PM43 splice abandoned",                5 },
    { 93, EmissionMode::Skip, "PM44: PM43 splice abandoned",                5 },
    { 94, EmissionMode::Skip, "PM37 OptB: #94",   5 },
    { 95, EmissionMode::Skip, "PM37 OptB: #95",   5 }, { 96, EmissionMode::Skip, "PM37 OptB: #96",   5 },
    { 97, EmissionMode::Skip, "PM37 OptB: #97",   5 }, { 98, EmissionMode::Skip, "PM37 OptB: #98",   5 },
    { 99, EmissionMode::Skip, "PM37 OptB: #99",   5 }, { 100, EmissionMode::Skip, "PM37 OptB: #100", 5 },
    { 101, EmissionMode::Skip, "PM37 OptB: #101", 5 }, { 102, EmissionMode::Skip, "PM37 OptB: #102", 5 },
    { 103, EmissionMode::Skip, "PM37 OptB: #103", 5 }, { 104, EmissionMode::Skip, "PM37 OptB: #104", 5 },
    { 105, EmissionMode::Skip, "PM37 OptB: #105", 5 }, { 106, EmissionMode::Skip, "PM37 OptB: #106", 5 },
    { 107, EmissionMode::Skip, "PM37 OptB: #107", 5 }, { 108, EmissionMode::Skip, "PM37 OptB: #108", 5 },
    { 109, EmissionMode::Skip, "PM37 OptB: #109", 5 }, { 110, EmissionMode::Skip, "PM37 OptB: #110", 5 },
    { 111, EmissionMode::Skip, "PM37 OptB: #111", 5 }, { 112, EmissionMode::Skip, "PM37 OptB: #112", 5 },
    { 113, EmissionMode::Skip, "PM37 OptB: #113", 5 }, { 114, EmissionMode::Skip, "PM37 OptB: #114", 5 },
    { 115, EmissionMode::Skip, "PM37 OptB: #115", 5 }, { 116, EmissionMode::Skip, "PM37 OptB: #116", 5 },
    { 117, EmissionMode::Skip, "PM37 OptB: #117", 5 }, { 118, EmissionMode::Skip, "PM37 OptB: #118", 5 },
    { 119, EmissionMode::Skip, "PM37 OptB: #119", 5 }, { 120, EmissionMode::Skip, "PM37 OptB: #120", 5 },
    { 121, EmissionMode::Skip, "PM37 OptB: #121", 5 }, { 122, EmissionMode::Skip, "PM37 OptB: #122", 5 },
    { 123, EmissionMode::Skip, "PM37 OptB: #123", 5 }, { 124, EmissionMode::Skip, "PM37 OptB: #124", 5 },
    { 125, EmissionMode::Skip, "PM37 OptB: #125", 5 }, { 126, EmissionMode::Skip, "PM37 OptB: #126", 5 },
    { 127, EmissionMode::Skip, "PM37 OptB: #127", 5 }, { 128, EmissionMode::Skip, "PM37 OptB: #128", 5 },
    { 129, EmissionMode::Skip, "PM37 OptB: #129", 5 }, { 130, EmissionMode::Skip, "PM37 OptB: #130", 5 },
    { 131, EmissionMode::Skip, "PM37 OptB: #131", 5 }, { 132, EmissionMode::Skip, "PM37 OptB: #132", 5 },
    { 133, EmissionMode::Skip, "PM37 OptB: #133", 5 },
    // ── pkt#134 — REDIRECTED 2026-04-28 PM (Phase B.0p4) ────────────────
    // Originally: Splice the captured ClientRestart RPC.  Audit revealed:
    //   1. The captured packet's bunches don't decode cleanly with our
    //      parser (parser sees ch=15247 garbage at file's bsb=152).  The
    //      client may receive it but get nothing useful.
    //   2. Even if it decoded, its Pawn NetGUID was for the original
    //      session's pawn, not ours.  AcknowledgePossession would fail.
    //   3. PawnEmitter::emit_captured() now fires a clean native
    //      ClientRestart immediately after pkt#78 ships, with NetGUID 88
    //      bound to the captured pawn the client just registered.
    //
    // Sending the captured pkt#134 in addition to our native CR risks:
    //   - Duplicate ClientRestart triggers AcknowledgePossession twice
    //   - Malformed bytes cascade into surrounding bunches → CNSF
    //
    // Skip this packet.  If it turns out post-CR the client expects more
    // data from this position, we'll re-enable as Splice OR build native.
    { 134, EmissionMode::Skip, "★ ClientRestart — now native via PawnEmitter", 5 },

    // ── Phase 7.6: Post-ClientRestart initial replication wave (pkts 135-149)
    //
    // After ClientRestart succeeds (timeout dialog elimination confirmed
    // 2026-04-27 15:38), the client transitions to "active streaming" mode
    // and requests many _Generated_/HASH tiles via SULV.  The captured
    // server's response WAS pkts 135-149 — initial property replication
    // for HP/MP/PlayerState/positions.  Without those, streaming subsystem
    // polls but never marks "all done" → loading screen loops forever.
    //
    // Splice through 135-149 to deliver the replication wave.
    { 135, EmissionMode::Skip, "PM37 OptB: skip post-CR rep #135", 5 },
    { 136, EmissionMode::Skip, "PM37 OptB: #136", 5 },
    { 137, EmissionMode::Skip, "PM37 OptB: #137", 5 },
    { 138, EmissionMode::Skip, "PM37 OptB: #138", 5 },
    { 139, EmissionMode::Skip, "PM37 OptB: #139", 5 },
    { 140, EmissionMode::Skip, "PM37 OptB: #140", 5 },
    { 141, EmissionMode::Skip, "PM37 OptB: #141", 5 },
    { 142, EmissionMode::Skip, "PM37 OptB: #142", 5 },
    { 143, EmissionMode::Skip, "PM37 OptB: #143", 5 },
    { 144, EmissionMode::Skip, "PM37 OptB: #144", 5 },
    { 145, EmissionMode::Skip, "PM37 OptB: #145", 5 },
    { 146, EmissionMode::Skip, "PM37 OptB: #146", 5 },
    { 147, EmissionMode::Skip, "PM37 OptB: #147", 5 },
    { 148, EmissionMode::Skip, "PM37 OptB: #148", 5 },
    { 149, EmissionMode::Skip, "PM37 OptB: #149", 5 },
// Cyb with C | O | A

    // ── Phase 7.7: Extended replication (pkts 150-180) — 2026-04-27 16:15 ──
    //
    // After RE'ing CULSS exec thunk (sub_144444850) and discovering AOC's
    // 7-param variant (vs stock UE5's 5-param), determined building native
    // CULSS is high-risk: 7 unknowns (wire_idx, FName encoding, 4 bool
    // values, mystery 5th param via sub_141712DD0, int LODIndex, bool).
    //
    // Captured server's pkts 150+ likely contain the CULSS calls and other
    // streaming-completion data PERFECTLY ENCODED.  Splice them.
    //
    // chSeq risk: post-ClientRestart (15:38 fix), the captured chSeq state
    // is now compatible with our session.  Earlier divergence theory may
    // not apply to this range.
    { 150, EmissionMode::Skip, "PM37 OptB: skip ext rep #150", 5 }, { 151, EmissionMode::Skip, "PM37 OptB: #151", 5 },
    { 152, EmissionMode::Skip, "PM37 OptB: #152", 5 }, { 153, EmissionMode::Skip, "PM37 OptB: #153", 5 },
    { 154, EmissionMode::Skip, "PM37 OptB: #154", 5 }, { 155, EmissionMode::Skip, "PM37 OptB: #155", 5 },
    { 156, EmissionMode::Skip, "PM37 OptB: #156", 5 }, { 157, EmissionMode::Skip, "PM37 OptB: #157", 5 },
    { 158, EmissionMode::Skip, "PM37 OptB: #158", 5 }, { 159, EmissionMode::Skip, "PM37 OptB: #159", 5 },
    { 160, EmissionMode::Skip, "PM37 OptB: #160", 5 }, { 161, EmissionMode::Skip, "PM37 OptB: #161", 5 },
    { 162, EmissionMode::Skip, "PM37 OptB: #162", 5 }, { 163, EmissionMode::Skip, "PM37 OptB: #163", 5 },
    { 164, EmissionMode::Skip, "PM37 OptB: #164", 5 }, { 165, EmissionMode::Skip, "PM37 OptB: #165", 5 },
    { 166, EmissionMode::Skip, "PM37 OptB: #166", 5 }, { 167, EmissionMode::Skip, "PM37 OptB: #167", 5 },
    { 168, EmissionMode::Skip, "PM37 OptB: #168", 5 }, { 169, EmissionMode::Skip, "PM37 OptB: #169", 5 },
    { 170, EmissionMode::Skip, "PM37 OptB: #170", 5 }, { 171, EmissionMode::Skip, "PM37 OptB: #171", 5 },
    { 172, EmissionMode::Skip, "PM37 OptB: #172", 5 }, { 173, EmissionMode::Skip, "PM37 OptB: #173", 5 },
    { 174, EmissionMode::Skip, "PM37 OptB: #174", 5 }, { 175, EmissionMode::Skip, "PM37 OptB: #175", 5 },
    { 176, EmissionMode::Skip, "PM37 OptB: #176", 5 }, { 177, EmissionMode::Skip, "PM37 OptB: #177", 5 },
    { 178, EmissionMode::Skip, "PM37 OptB: #178", 5 }, { 179, EmissionMode::Skip, "PM37 OptB: #179", 5 },
    { 180, EmissionMode::Skip, "PM37 OptB: #180", 5 },

    // ── Phase 7.8: Deep splice (pkts 181-500) — 2026-04-27 16:50 ─────────
    //
    // Per RE'd sub_1447AE890 (the IsStreamingFinished impl): the gate fires
    // when ALL entries in array a1+184 pass sub_144789190 (per-cell ready).
    // Client log shows persistent "1 QueuedPackages" — exactly ONE cell
    // failing the ready check.
    //
    // PM37 (2026-04-29) Option B: was DEEP_SPLICE, now DEEP_SKIP.
    // Every captured packet referencing captured-session NetGUIDs would
    // hit ContentBlockFail like Node_Villager_F_C did in PM36.  Skip them
    // all and reintroduce native equivalents only as we need them.
    #define DEEP_SPLICE(N) { N, EmissionMode::Skip, "PM37 OptB: skip deep rep #" #N, 5 }
    DEEP_SPLICE(181), DEEP_SPLICE(182), DEEP_SPLICE(183), DEEP_SPLICE(184), DEEP_SPLICE(185),
    DEEP_SPLICE(186), DEEP_SPLICE(187), DEEP_SPLICE(188), DEEP_SPLICE(189), DEEP_SPLICE(190),
    DEEP_SPLICE(191), DEEP_SPLICE(192), DEEP_SPLICE(193), DEEP_SPLICE(194), DEEP_SPLICE(195),
    DEEP_SPLICE(196), DEEP_SPLICE(197), DEEP_SPLICE(198), DEEP_SPLICE(199), DEEP_SPLICE(200),
    DEEP_SPLICE(201), DEEP_SPLICE(202), DEEP_SPLICE(203), DEEP_SPLICE(204), DEEP_SPLICE(205),
    DEEP_SPLICE(206), DEEP_SPLICE(207), DEEP_SPLICE(208), DEEP_SPLICE(209), DEEP_SPLICE(210),
    DEEP_SPLICE(211), DEEP_SPLICE(212), DEEP_SPLICE(213), DEEP_SPLICE(214), DEEP_SPLICE(215),
    DEEP_SPLICE(216), DEEP_SPLICE(217), DEEP_SPLICE(218), DEEP_SPLICE(219), DEEP_SPLICE(220),
    DEEP_SPLICE(221), DEEP_SPLICE(222), DEEP_SPLICE(223), DEEP_SPLICE(224), DEEP_SPLICE(225),
    DEEP_SPLICE(226), DEEP_SPLICE(227), DEEP_SPLICE(228), DEEP_SPLICE(229), DEEP_SPLICE(230),
    DEEP_SPLICE(231), DEEP_SPLICE(232), DEEP_SPLICE(233), DEEP_SPLICE(234), DEEP_SPLICE(235),
    DEEP_SPLICE(236), DEEP_SPLICE(237), DEEP_SPLICE(238), DEEP_SPLICE(239), DEEP_SPLICE(240),
    DEEP_SPLICE(241), DEEP_SPLICE(242), DEEP_SPLICE(243), DEEP_SPLICE(244), DEEP_SPLICE(245),
    DEEP_SPLICE(246), DEEP_SPLICE(247), DEEP_SPLICE(248), DEEP_SPLICE(249), DEEP_SPLICE(250),
    DEEP_SPLICE(251), DEEP_SPLICE(252), DEEP_SPLICE(253), DEEP_SPLICE(254), DEEP_SPLICE(255),
    DEEP_SPLICE(256), DEEP_SPLICE(257), DEEP_SPLICE(258), DEEP_SPLICE(259), DEEP_SPLICE(260),
    DEEP_SPLICE(261), DEEP_SPLICE(262), DEEP_SPLICE(263), DEEP_SPLICE(264), DEEP_SPLICE(265),
    DEEP_SPLICE(266), DEEP_SPLICE(267), DEEP_SPLICE(268), DEEP_SPLICE(269), DEEP_SPLICE(270),
    DEEP_SPLICE(271), DEEP_SPLICE(272), DEEP_SPLICE(273), DEEP_SPLICE(274), DEEP_SPLICE(275),
    DEEP_SPLICE(276), DEEP_SPLICE(277), DEEP_SPLICE(278), DEEP_SPLICE(279), DEEP_SPLICE(280),
    DEEP_SPLICE(281), DEEP_SPLICE(282), DEEP_SPLICE(283), DEEP_SPLICE(284), DEEP_SPLICE(285),
    DEEP_SPLICE(286), DEEP_SPLICE(287), DEEP_SPLICE(288), DEEP_SPLICE(289), DEEP_SPLICE(290),
    DEEP_SPLICE(291), DEEP_SPLICE(292), DEEP_SPLICE(293), DEEP_SPLICE(294), DEEP_SPLICE(295),
    DEEP_SPLICE(296), DEEP_SPLICE(297), DEEP_SPLICE(298), DEEP_SPLICE(299), DEEP_SPLICE(300),
    DEEP_SPLICE(301), DEEP_SPLICE(302), DEEP_SPLICE(303), DEEP_SPLICE(304), DEEP_SPLICE(305),
    DEEP_SPLICE(306), DEEP_SPLICE(307), DEEP_SPLICE(308), DEEP_SPLICE(309), DEEP_SPLICE(310),
    DEEP_SPLICE(311), DEEP_SPLICE(312), DEEP_SPLICE(313), DEEP_SPLICE(314), DEEP_SPLICE(315),
    DEEP_SPLICE(316), DEEP_SPLICE(317), DEEP_SPLICE(318), DEEP_SPLICE(319), DEEP_SPLICE(320),
    DEEP_SPLICE(321), DEEP_SPLICE(322), DEEP_SPLICE(323), DEEP_SPLICE(324), DEEP_SPLICE(325),
    DEEP_SPLICE(326), DEEP_SPLICE(327), DEEP_SPLICE(328), DEEP_SPLICE(329), DEEP_SPLICE(330),
    DEEP_SPLICE(331), DEEP_SPLICE(332), DEEP_SPLICE(333), DEEP_SPLICE(334), DEEP_SPLICE(335),
    DEEP_SPLICE(336), DEEP_SPLICE(337), DEEP_SPLICE(338), DEEP_SPLICE(339), DEEP_SPLICE(340),
    DEEP_SPLICE(341), DEEP_SPLICE(342), DEEP_SPLICE(343), DEEP_SPLICE(344), DEEP_SPLICE(345),
    DEEP_SPLICE(346), DEEP_SPLICE(347), DEEP_SPLICE(348), DEEP_SPLICE(349), DEEP_SPLICE(350),
    DEEP_SPLICE(351), DEEP_SPLICE(352), DEEP_SPLICE(353), DEEP_SPLICE(354), DEEP_SPLICE(355),
    DEEP_SPLICE(356), DEEP_SPLICE(357), DEEP_SPLICE(358), DEEP_SPLICE(359), DEEP_SPLICE(360),
    DEEP_SPLICE(361), DEEP_SPLICE(362), DEEP_SPLICE(363), DEEP_SPLICE(364), DEEP_SPLICE(365),
    DEEP_SPLICE(366), DEEP_SPLICE(367), DEEP_SPLICE(368), DEEP_SPLICE(369), DEEP_SPLICE(370),
    DEEP_SPLICE(371), DEEP_SPLICE(372), DEEP_SPLICE(373), DEEP_SPLICE(374), DEEP_SPLICE(375),
    DEEP_SPLICE(376), DEEP_SPLICE(377), DEEP_SPLICE(378), DEEP_SPLICE(379), DEEP_SPLICE(380),
    DEEP_SPLICE(381), DEEP_SPLICE(382), DEEP_SPLICE(383), DEEP_SPLICE(384), DEEP_SPLICE(385),
    DEEP_SPLICE(386), DEEP_SPLICE(387), DEEP_SPLICE(388), DEEP_SPLICE(389), DEEP_SPLICE(390),
    DEEP_SPLICE(391), DEEP_SPLICE(392), DEEP_SPLICE(393), DEEP_SPLICE(394), DEEP_SPLICE(395),
    DEEP_SPLICE(396), DEEP_SPLICE(397), DEEP_SPLICE(398), DEEP_SPLICE(399), DEEP_SPLICE(400),
    DEEP_SPLICE(401), DEEP_SPLICE(402), DEEP_SPLICE(403), DEEP_SPLICE(404), DEEP_SPLICE(405),
    DEEP_SPLICE(406), DEEP_SPLICE(407), DEEP_SPLICE(408), DEEP_SPLICE(409), DEEP_SPLICE(410),
    DEEP_SPLICE(411), DEEP_SPLICE(412), DEEP_SPLICE(413), DEEP_SPLICE(414), DEEP_SPLICE(415),
    DEEP_SPLICE(416), DEEP_SPLICE(417), DEEP_SPLICE(418), DEEP_SPLICE(419), DEEP_SPLICE(420),
    DEEP_SPLICE(421), DEEP_SPLICE(422), DEEP_SPLICE(423), DEEP_SPLICE(424), DEEP_SPLICE(425),
    DEEP_SPLICE(426), DEEP_SPLICE(427), DEEP_SPLICE(428), DEEP_SPLICE(429), DEEP_SPLICE(430),
    DEEP_SPLICE(431), DEEP_SPLICE(432), DEEP_SPLICE(433), DEEP_SPLICE(434), DEEP_SPLICE(435),
    DEEP_SPLICE(436), DEEP_SPLICE(437), DEEP_SPLICE(438), DEEP_SPLICE(439), DEEP_SPLICE(440),
    DEEP_SPLICE(441), DEEP_SPLICE(442), DEEP_SPLICE(443), DEEP_SPLICE(444), DEEP_SPLICE(445),
    DEEP_SPLICE(446), DEEP_SPLICE(447), DEEP_SPLICE(448), DEEP_SPLICE(449), DEEP_SPLICE(450),
    DEEP_SPLICE(451), DEEP_SPLICE(452), DEEP_SPLICE(453), DEEP_SPLICE(454), DEEP_SPLICE(455),
    DEEP_SPLICE(456), DEEP_SPLICE(457), DEEP_SPLICE(458), DEEP_SPLICE(459), DEEP_SPLICE(460),
    DEEP_SPLICE(461), DEEP_SPLICE(462), DEEP_SPLICE(463), DEEP_SPLICE(464), DEEP_SPLICE(465),
    DEEP_SPLICE(466), DEEP_SPLICE(467), DEEP_SPLICE(468), DEEP_SPLICE(469), DEEP_SPLICE(470),
    DEEP_SPLICE(471), DEEP_SPLICE(472), DEEP_SPLICE(473), DEEP_SPLICE(474), DEEP_SPLICE(475),
    DEEP_SPLICE(476), DEEP_SPLICE(477), DEEP_SPLICE(478), DEEP_SPLICE(479), DEEP_SPLICE(480),
    DEEP_SPLICE(481), DEEP_SPLICE(482), DEEP_SPLICE(483), DEEP_SPLICE(484), DEEP_SPLICE(485),
    DEEP_SPLICE(486), DEEP_SPLICE(487), DEEP_SPLICE(488), DEEP_SPLICE(489), DEEP_SPLICE(490),
    DEEP_SPLICE(491), DEEP_SPLICE(492), DEEP_SPLICE(493), DEEP_SPLICE(494), DEEP_SPLICE(495),
    DEEP_SPLICE(496), DEEP_SPLICE(497), DEEP_SPLICE(498), DEEP_SPLICE(499), DEEP_SPLICE(500),
    #undef DEEP_SPLICE
#endif
// (C.O.A ~ Cyb)

    // ── Phase 8: Initial replication tick (HP/MP/PlayerState/positions) ──
#if !AOC_OPTION_C_MINIMAL
    { 79, EmissionMode::Splice, "initial replication #79", 30 },
    { 80, EmissionMode::Splice, "actor #80", 30 }, { 81, EmissionMode::Splice, "actor #81", 30 },
    { 82, EmissionMode::Splice, "actor #82", 30 }, { 83, EmissionMode::Splice, "actor #83", 30 },
    { 84, EmissionMode::Splice, "actor #84", 30 }, { 85, EmissionMode::Splice, "actor #85", 30 },
    { 86, EmissionMode::Splice, "actor #86", 30 }, { 87, EmissionMode::Splice, "actor #87", 30 },
    { 88, EmissionMode::Splice, "actor #88", 30 }, { 89, EmissionMode::Splice, "actor #89", 30 },
    { 90, EmissionMode::Splice, "actor #90", 30 }, { 91, EmissionMode::Splice, "actor #91", 30 },
    { 92, EmissionMode::Splice, "actor #92", 30 }, { 93, EmissionMode::Splice, "actor #93", 30 },
    { 94, EmissionMode::Splice, "actor #94", 30 }, { 95, EmissionMode::Splice, "actor #95", 30 },
    { 96, EmissionMode::Splice, "actor #96", 30 }, { 97, EmissionMode::Splice, "actor #97", 30 },
    { 98, EmissionMode::Splice, "actor #98", 30 }, { 99, EmissionMode::Splice, "actor #99", 30 },

    // ── Phase 9: Stretch to ~150 (matches the empirical "100+ packets"
    //    finding from docs/NATIVE-EMISSION-ARCHITECTURE.md) ──
    { 100, EmissionMode::Splice, "actor #100", 30 }, { 101, EmissionMode::Splice, "actor #101", 30 },
    { 102, EmissionMode::Splice, "actor #102", 30 }, { 103, EmissionMode::Splice, "actor #103", 30 },
    { 104, EmissionMode::Splice, "Name update? #104", 30 }, { 105, EmissionMode::Splice, "actor #105", 30 },
    { 106, EmissionMode::Splice, "actor #106", 30 }, { 107, EmissionMode::Splice, "actor #107", 30 },
    { 108, EmissionMode::Splice, "actor #108", 30 }, { 109, EmissionMode::Splice, "actor #109", 30 },
    { 110, EmissionMode::Splice, "actor #110", 30 }, { 111, EmissionMode::Splice, "actor #111", 30 },
    { 112, EmissionMode::Splice, "actor #112", 30 }, { 113, EmissionMode::Splice, "actor #113", 30 },
    { 114, EmissionMode::Splice, "actor #114", 30 }, { 115, EmissionMode::Splice, "actor #115", 30 },
    { 116, EmissionMode::Splice, "actor #116", 30 }, { 117, EmissionMode::Splice, "actor #117", 30 },
    { 118, EmissionMode::Splice, "actor #118", 30 }, { 119, EmissionMode::Splice, "actor #119", 30 },
    { 120, EmissionMode::Splice, "actor #120", 30 }, { 121, EmissionMode::Splice, "actor #121", 30 },
    { 122, EmissionMode::Splice, "actor #122", 30 }, { 123, EmissionMode::Splice, "actor #123", 30 },
    { 124, EmissionMode::Splice, "actor #124", 30 }, { 125, EmissionMode::Splice, "actor #125", 30 },
    { 126, EmissionMode::Splice, "actor #126", 30 }, { 127, EmissionMode::Splice, "actor #127", 30 },
    { 128, EmissionMode::Splice, "actor #128", 30 }, { 129, EmissionMode::Splice, "actor #129", 30 },
    { 130, EmissionMode::Splice, "actor #130", 30 }, { 131, EmissionMode::Splice, "actor #131", 30 },
    { 132, EmissionMode::Splice, "actor #132", 30 }, { 133, EmissionMode::Splice, "actor #133", 30 },
    { 134, EmissionMode::Splice, "actor #134", 30 }, { 135, EmissionMode::Splice, "actor #135", 30 },
    { 136, EmissionMode::Splice, "actor #136", 30 }, { 137, EmissionMode::Splice, "actor #137", 30 },
    { 138, EmissionMode::Splice, "actor #138", 30 }, { 139, EmissionMode::Splice, "actor #139", 30 },
    { 140, EmissionMode::Splice, "actor #140", 30 }, { 141, EmissionMode::Splice, "actor #141", 30 },
    { 142, EmissionMode::Splice, "actor #142", 30 }, { 143, EmissionMode::Splice, "actor #143", 30 },
    { 144, EmissionMode::Splice, "actor #144", 30 }, { 145, EmissionMode::Splice, "actor #145", 30 },
    { 146, EmissionMode::Splice, "actor #146", 30 }, { 147, EmissionMode::Splice, "actor #147", 30 },
    { 148, EmissionMode::Splice, "actor #148", 30 }, { 149, EmissionMode::Splice, "actor #149", 30 },

    // ── Phase 10: World subobject state replication (pkts 150-499) ───────
    //
    // 2026-04-26 Phase B.0b — empirical finding from emu-20260426-185511.log:
    // after the first 150 packets, the client floods us with C>S
    // PackageMap exports for `_Generated_/...` subobjects in
    // Verra_World_Master, e.g.:
    //   ch=5377  '/Game/Levels/.../_Generated_/COTXYV4DKJERCR6YFUCML54PT'
    //   ch=5889  '/Game/Levels/.../_Generated_/7U8EVAYQU1KTPISRJDVFHVQF7'
    //
    // These mean: "I've loaded all the streaming chunks (100%) — please
    // send me the actor state for these subobjects so I can finish
    // loading."  Without an answer the client loops on
    // "Waiting for World Partition Streaming 100%".
    //
    // The captured replay's pkts 150+ contain initial property
    // replication for these subobjects (NPCs, static actors, props).
    // Splice them through to satisfy the client's state requests.
    // 350 more packets (~6.4s of additional emission at 18ms cadence).
    // Lighter pacing (10ms) to keep total bootstrap window comparable.
    // 2026-04-26 Phase B.0c — pacing fix.  In the proven hybrid 10-min
    // test, replay_loop emitted 148 packets in 22.7 s (≈153 ms/pkt avg
    // via adaptive 1-50ms tier pacing).  Our first attempt sent 478
    // packets in 10 s (≈20 ms/pkt) — 7.5× faster than baseline — and
    // produced flashing-then-reload symptoms (client can't keep up,
    // internal "still loading" timer fires, world resets).  Using
    // 50 ms here gives ~17.5 s for 350 extra rows, total bootstrap
    // window ≈22 s — comparable to the working hybrid baseline.
    #define SPLICE_ROW(N) { N, EmissionMode::Splice, "world replication #" #N, 50 }
    SPLICE_ROW(150), SPLICE_ROW(151), SPLICE_ROW(152), SPLICE_ROW(153), SPLICE_ROW(154),
    SPLICE_ROW(155), SPLICE_ROW(156), SPLICE_ROW(157), SPLICE_ROW(158), SPLICE_ROW(159),
    SPLICE_ROW(160), SPLICE_ROW(161), SPLICE_ROW(162), SPLICE_ROW(163), SPLICE_ROW(164),
    SPLICE_ROW(165), SPLICE_ROW(166), SPLICE_ROW(167), SPLICE_ROW(168), SPLICE_ROW(169),
    SPLICE_ROW(170), SPLICE_ROW(171), SPLICE_ROW(172), SPLICE_ROW(173), SPLICE_ROW(174),
    SPLICE_ROW(175), SPLICE_ROW(176), SPLICE_ROW(177), SPLICE_ROW(178), SPLICE_ROW(179),
    SPLICE_ROW(180), SPLICE_ROW(181), SPLICE_ROW(182), SPLICE_ROW(183), SPLICE_ROW(184),
    SPLICE_ROW(185), SPLICE_ROW(186), SPLICE_ROW(187), SPLICE_ROW(188), SPLICE_ROW(189),
    SPLICE_ROW(190), SPLICE_ROW(191), SPLICE_ROW(192), SPLICE_ROW(193), SPLICE_ROW(194),
    SPLICE_ROW(195), SPLICE_ROW(196), SPLICE_ROW(197), SPLICE_ROW(198), SPLICE_ROW(199),
    SPLICE_ROW(200), SPLICE_ROW(201), SPLICE_ROW(202), SPLICE_ROW(203), SPLICE_ROW(204),
    SPLICE_ROW(205), SPLICE_ROW(206), SPLICE_ROW(207), SPLICE_ROW(208), SPLICE_ROW(209),
    SPLICE_ROW(210), SPLICE_ROW(211), SPLICE_ROW(212), SPLICE_ROW(213), SPLICE_ROW(214),
    SPLICE_ROW(215), SPLICE_ROW(216), SPLICE_ROW(217), SPLICE_ROW(218), SPLICE_ROW(219),
    SPLICE_ROW(220), SPLICE_ROW(221), SPLICE_ROW(222), SPLICE_ROW(223), SPLICE_ROW(224),
    SPLICE_ROW(225), SPLICE_ROW(226), SPLICE_ROW(227), SPLICE_ROW(228), SPLICE_ROW(229),
    SPLICE_ROW(230), SPLICE_ROW(231), SPLICE_ROW(232), SPLICE_ROW(233), SPLICE_ROW(234),
    SPLICE_ROW(235), SPLICE_ROW(236), SPLICE_ROW(237), SPLICE_ROW(238), SPLICE_ROW(239),
    SPLICE_ROW(240), SPLICE_ROW(241), SPLICE_ROW(242), SPLICE_ROW(243), SPLICE_ROW(244),
    SPLICE_ROW(245), SPLICE_ROW(246), SPLICE_ROW(247), SPLICE_ROW(248), SPLICE_ROW(249),
    SPLICE_ROW(250), SPLICE_ROW(251), SPLICE_ROW(252), SPLICE_ROW(253), SPLICE_ROW(254),
    SPLICE_ROW(255), SPLICE_ROW(256), SPLICE_ROW(257), SPLICE_ROW(258), SPLICE_ROW(259),
    SPLICE_ROW(260), SPLICE_ROW(261), SPLICE_ROW(262), SPLICE_ROW(263), SPLICE_ROW(264),
    SPLICE_ROW(265), SPLICE_ROW(266), SPLICE_ROW(267), SPLICE_ROW(268), SPLICE_ROW(269),
    SPLICE_ROW(270), SPLICE_ROW(271), SPLICE_ROW(272), SPLICE_ROW(273), SPLICE_ROW(274),
    SPLICE_ROW(275), SPLICE_ROW(276), SPLICE_ROW(277), SPLICE_ROW(278), SPLICE_ROW(279),
    SPLICE_ROW(280), SPLICE_ROW(281), SPLICE_ROW(282), SPLICE_ROW(283), SPLICE_ROW(284),
    SPLICE_ROW(285), SPLICE_ROW(286), SPLICE_ROW(287), SPLICE_ROW(288), SPLICE_ROW(289),
    SPLICE_ROW(290), SPLICE_ROW(291), SPLICE_ROW(292), SPLICE_ROW(293), SPLICE_ROW(294),
    SPLICE_ROW(295), SPLICE_ROW(296), SPLICE_ROW(297), SPLICE_ROW(298), SPLICE_ROW(299),
    SPLICE_ROW(300), SPLICE_ROW(301), SPLICE_ROW(302), SPLICE_ROW(303), SPLICE_ROW(304),
    SPLICE_ROW(305), SPLICE_ROW(306), SPLICE_ROW(307), SPLICE_ROW(308), SPLICE_ROW(309),
    SPLICE_ROW(310), SPLICE_ROW(311), SPLICE_ROW(312), SPLICE_ROW(313), SPLICE_ROW(314),
    SPLICE_ROW(315), SPLICE_ROW(316), SPLICE_ROW(317), SPLICE_ROW(318), SPLICE_ROW(319),
    SPLICE_ROW(320), SPLICE_ROW(321), SPLICE_ROW(322), SPLICE_ROW(323), SPLICE_ROW(324),
    SPLICE_ROW(325), SPLICE_ROW(326), SPLICE_ROW(327), SPLICE_ROW(328), SPLICE_ROW(329),
    SPLICE_ROW(330), SPLICE_ROW(331), SPLICE_ROW(332), SPLICE_ROW(333), SPLICE_ROW(334),
    SPLICE_ROW(335), SPLICE_ROW(336), SPLICE_ROW(337), SPLICE_ROW(338), SPLICE_ROW(339),
    SPLICE_ROW(340), SPLICE_ROW(341), SPLICE_ROW(342), SPLICE_ROW(343), SPLICE_ROW(344),
    SPLICE_ROW(345), SPLICE_ROW(346), SPLICE_ROW(347), SPLICE_ROW(348), SPLICE_ROW(349),
    SPLICE_ROW(350), SPLICE_ROW(351), SPLICE_ROW(352), SPLICE_ROW(353), SPLICE_ROW(354),
    SPLICE_ROW(355), SPLICE_ROW(356), SPLICE_ROW(357), SPLICE_ROW(358), SPLICE_ROW(359),
    SPLICE_ROW(360), SPLICE_ROW(361), SPLICE_ROW(362), SPLICE_ROW(363), SPLICE_ROW(364),
    SPLICE_ROW(365), SPLICE_ROW(366), SPLICE_ROW(367), SPLICE_ROW(368), SPLICE_ROW(369),
    SPLICE_ROW(370), SPLICE_ROW(371), SPLICE_ROW(372), SPLICE_ROW(373), SPLICE_ROW(374),
    SPLICE_ROW(375), SPLICE_ROW(376), SPLICE_ROW(377), SPLICE_ROW(378), SPLICE_ROW(379),
    SPLICE_ROW(380), SPLICE_ROW(381), SPLICE_ROW(382), SPLICE_ROW(383), SPLICE_ROW(384),
    SPLICE_ROW(385), SPLICE_ROW(386), SPLICE_ROW(387), SPLICE_ROW(388), SPLICE_ROW(389),
    SPLICE_ROW(390), SPLICE_ROW(391), SPLICE_ROW(392), SPLICE_ROW(393), SPLICE_ROW(394),
    SPLICE_ROW(395), SPLICE_ROW(396), SPLICE_ROW(397), SPLICE_ROW(398), SPLICE_ROW(399),
    SPLICE_ROW(400), SPLICE_ROW(401), SPLICE_ROW(402), SPLICE_ROW(403), SPLICE_ROW(404),
    SPLICE_ROW(405), SPLICE_ROW(406), SPLICE_ROW(407), SPLICE_ROW(408), SPLICE_ROW(409),
    SPLICE_ROW(410), SPLICE_ROW(411), SPLICE_ROW(412), SPLICE_ROW(413), SPLICE_ROW(414),
    SPLICE_ROW(415), SPLICE_ROW(416), SPLICE_ROW(417), SPLICE_ROW(418), SPLICE_ROW(419),
    SPLICE_ROW(420), SPLICE_ROW(421), SPLICE_ROW(422), SPLICE_ROW(423), SPLICE_ROW(424),
    SPLICE_ROW(425), SPLICE_ROW(426), SPLICE_ROW(427), SPLICE_ROW(428), SPLICE_ROW(429),
    SPLICE_ROW(430), SPLICE_ROW(431), SPLICE_ROW(432), SPLICE_ROW(433), SPLICE_ROW(434),
    SPLICE_ROW(435), SPLICE_ROW(436), SPLICE_ROW(437), SPLICE_ROW(438), SPLICE_ROW(439),
    SPLICE_ROW(440), SPLICE_ROW(441), SPLICE_ROW(442), SPLICE_ROW(443), SPLICE_ROW(444),
    SPLICE_ROW(445), SPLICE_ROW(446), SPLICE_ROW(447), SPLICE_ROW(448), SPLICE_ROW(449),
    SPLICE_ROW(450), SPLICE_ROW(451), SPLICE_ROW(452), SPLICE_ROW(453), SPLICE_ROW(454),
    SPLICE_ROW(455), SPLICE_ROW(456), SPLICE_ROW(457), SPLICE_ROW(458), SPLICE_ROW(459),
    SPLICE_ROW(460), SPLICE_ROW(461), SPLICE_ROW(462), SPLICE_ROW(463), SPLICE_ROW(464),
    SPLICE_ROW(465), SPLICE_ROW(466), SPLICE_ROW(467), SPLICE_ROW(468), SPLICE_ROW(469),
    SPLICE_ROW(470), SPLICE_ROW(471), SPLICE_ROW(472), SPLICE_ROW(473), SPLICE_ROW(474),
    SPLICE_ROW(475), SPLICE_ROW(476), SPLICE_ROW(477), SPLICE_ROW(478), SPLICE_ROW(479),
    SPLICE_ROW(480), SPLICE_ROW(481), SPLICE_ROW(482), SPLICE_ROW(483), SPLICE_ROW(484),
    SPLICE_ROW(485), SPLICE_ROW(486), SPLICE_ROW(487), SPLICE_ROW(488), SPLICE_ROW(489),
    SPLICE_ROW(490), SPLICE_ROW(491), SPLICE_ROW(492), SPLICE_ROW(493), SPLICE_ROW(494),
    SPLICE_ROW(495), SPLICE_ROW(496), SPLICE_ROW(497), SPLICE_ROW(498), SPLICE_ROW(499),
    #undef SPLICE_ROW
#endif // !AOC_OPTION_C_MINIMAL  (closes Phase 8/9/10 strip, opened above row #79)
};

}} // namespace aoc::net
