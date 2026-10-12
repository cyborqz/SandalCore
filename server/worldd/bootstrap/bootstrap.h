// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  bootstrap/bootstrap.h
//
//  The native bootstrap of one joining player (plan A5, spec section 4): the
//  sequence of chains worldd sends from NMT_Join to a client that can walk, as
//  data, built from a PlayerSetup with the libaocnet replicator. Pure and
//  offline: no sockets, no Connection, no database. Plan E3 sends each Send on a
//  channel of its role and feeds the acknowledgements back.
//
//  The sequence (join_sequence(); the original server's order, transcript
//  section 1.3; each step is also a method of its own):
//
//     1  PlayerController open: 7 properties + the GlobalGMCommands subobject (pkt#22)
//     2  ClientSetHUD(BP_AOCHUD_C) and its class export (pkt#22)
//     3  ClientVoiceHandshakeComplete, ClientEnableNetworkVoice(false) and the
//        captured level streaming list (pkt#23); ClientFlushLevelStreaming,
//        ClientCapBandwidth and ClientSetTime (pkt#23)
//     4  GameState open, by mode (below)
//     5  CharacterLoadTracker 1011...1 (PlayerController update; pkt#72)
//     6  the pawn's first state: the open with its 8 components (pkt#78), then
//        the four storage payloads (pkt#82-97) -- and, plan B2, the items: in the retail login's order (research A1), for each of
//        the Backpack, the Equipment and the Quest storage the PlayerController's ClientReceiveStorageEvent Add of each of its items,
//        then the storage's payload on the pawn (the Backpack's Adds go before the pawn's open)
//     7  ClientRestart(pawn), ClientSetCameraMode, ClientSetRotation (pkt#97)
//     8  PlayerController update: Pawn, ControllersOriginalPawn, Name, the attachment
//        to the pawn's CollisionCylinder and CharacterLoadTracker 0111...1 (pkt#103)
//     9  the pawn's second state: RemoteRole 2, Instigator, PlayerState, Controller,
//        bHidden 1, Alignment, Ability, Stats, the dynamic ResourceStorageComponent and
//        what the components gain (pkt#107)
//    10  PlayerState open (pkt#434)
//
//  Left out, as the spec says: the PlayerController's quest, FTUE and XP subobjects,
//  inventory items and storage events, world actors, the two unreliable load tracker
//  values of pkt#46 and #50, per-connection replication conditions.
//
//  What E3 sends on request (the replies, transcript section 2.2): phase_in(),
//  combat_settings_echo(), storage_payload(), empty_group(), default_action_bar(),
//  good_move_ack(), correction() (plan A6).
//
//  Replication state.  A PlayerBootstrap owns what the connection has of its four
//  actors (the replicator's ActorShadow) and the export acknowledgements. Every
//  method that sends advances them: the caller's contract is the replicator's, "emit,
//  then commit only when the chain was accepted": take a checkpoint() first and
//  restore() it when Connection::send_chain refuses what was built. mark_exported()
//  is how acknowledgements come in: once the packet carrying export bunch i of a Send
//  is acked, mark Send::emission.exported_by_bunch[i] (libs/aocnet/README.md "Caller
//  contracts"). A join_sequence() built before any acknowledgement exports a shared
//  object (the level, a class) in every chain that names it, as the original did.
//
//  Chain limits (net/connection.h): every chain here has at most 256 parts and every
//  export bunch at most 7621 bits; test_worldd_bootstrap checks it for the whole
//  sequence.
// ============================================================================
#pragma once

#include "bootstrap/ability_replication.h"
#include "bootstrap/constants.h"
#include "bootstrap/player_setup.h"
#include "protocol/repl/leveling.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/skill_rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/vendor_rpc.h"

#include <array>
#include <map>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd::bootstrap {

/// The object a chain is for. E3 gives each of the four its own actor channel.
enum class Role : uint8_t { PlayerController, GameState, Pawn, PlayerState };
// C | O | A + Cyb

/// How much of the original's GameState open worldd sends (transcript section 6.1; each tier is an in-game run).
enum class GameStateMode : uint8_t {
    /// AuthServerIDReplicated, RemoteRole, Role, GameModeClass, bReplicatedHasBegunPlay and the world time; empty
    /// world arrays.
    Tier1,
    /// Tier 1 plus the 25 CombatSettings values, GameTimeScalar and ServiceConnectionFlags (captured_gamestate.h).
    Tier2,
    /// Nodes 1, stage 2: Tier 2 plus the node maps retail's GameState carries (NodeIDToClientNodeData for the 46 nodes, the progress and citizens maps, the 244 estates:
    /// capture 1198 line 47) and the node fields of the pawn's OwnerInfo (CharactersNodeData).  Without the maps the client logs a "Record not found: CityNodeRecord"
    /// for every zone banner and finds no node for its audio.  The maps come from PlayerSetup::nodes and follow the world's node levels (node_maps_update).
    Tier3,
    /// ESCAPE HATCH FOR IN-GAME DEBUGGING ONLY. The original's whole GameState open verbatim: the real world's 244
    /// estates, 46 nodes and 32 quests, its actor GUID (4608, ServerId 60), its world time. Not native, not
    /// something the game server may ship, and a shadow that never learned of it: no update of the GameState
    /// follows. Use it to tell whether a missing piece of the native GameState is what stops the client.
    Captured,
};

/// One chain to send.
struct Send {
    Role              role = Role::PlayerController;
    bool              opens_channel = false;   // the first chain of the role's actor channel
    const char*       what = "";               // for logs and tests
    repl::Emission    emission;                // exports, payload, exported_by_bunch, reliable
    /// Plan D2, review: not one of the four players' actors but a lingering zone's (PlayerBootstrap::lingering_*): the ObjectId of its actor. The
    /// session gives each such actor a channel of its own (`role` means nothing then).
    uint64_t          effect_actor = 0;
    /// The mounts milestone, stage 3: set, the actor channel the chain goes on instead of its role's (a possessed mount's move acknowledgements and
    /// corrections: the actor channel of an actor the world's registry holds). 0: the role's channel.
    uint32_t          channel = 0;

    bool reliable() const { return emission.reliable; }
};

/// Plan D2, review: what the server replicates for a lingering zone (a Bard's dance): an AAoCAbilityLingeringEffect actor attached to the pawn,
/// which the client draws the zone's visual effect from [R: retail capture 44: 2,044 bits in two parts, SourceRecordGuid the ability,
/// LingeringEffectId the zone, two random FGuids, the pawn as Instigator and AttachParent, its CharacterMesh0 as AttachComponent; three
/// subobjects: LingeringEffectMovementComp {bIsActive 1}, AbilityComp and StatsComp, both empty]. The caller mints the guids.
struct LingeringSpec {
    emit::FIntrepidNetworkGUID actor, movement, ability, stats;   // the actor and its three subobjects
    emit::FIntrepidNetworkGUID pawn_mesh;                          // the pawn's CharacterMesh0: one guid for the whole session
    int64_t                    zone = 0;                           // LingeringEffectId
    int64_t                    source = 0;                         // SourceRecordGuid: the ability
    double                     x = 0, y = 0, z = 0;                // where it opens, cm
    double                     yaw_degrees = 0;
    std::optional<repl::QuantVec> velocity;                        // SerializeNewActor's velocity as the wire holds it (the retail actor had one; the session sends none)
    uint32_t                   effect_guid[4] = {0, 0, 0, 0};      // LingeringEffectGuid (an FGuid of its own)
    uint32_t                   instance_guid[4] = {0, 0, 0, 0};    // LingeringZoneInstGuid
};

/// One change of a storage the client is told of (plan B2): an Add or a Remove of one item in one slot.
struct ItemEvent {
    repl::StorageEventType type = repl::StorageEventType::Add;
    ItemStorage            storage = ItemStorage::Backpack;
    int32_t                slot = 0;
    repl::ItemInstance     item;
    bool                   instigated = true;   // the character's guid is the event's instigator, else the zero guid
};

/// One item leaving a loot container (plan E1, stage 3, part 2): the slot of the window it was in and the stack.
struct LootRemoval {
    int32_t            slot = 0;
    repl::ItemInstance item;
};

/// The most ClientReceiveStorageEvent calls one chain carries (a Backpack's 70 Adds go out in five).
inline constexpr size_t kMaxEventsPerChain = 16;

/// A property of an actor by its schema path and the value to set ("CharacterInGameSettings.CombatSettings.Input.BufferDuration").
struct PropertyValue {
    std::string_view path;
    repl::Value      value;
};

class PlayerBootstrap {
public:
    /// Builds the four actors from `setup`. ok() is false, with error() saying which value was refused, when the
    /// replicator will not carry one of them (a string with a NUL, a quantized component over 63 bits ...).
    PlayerBootstrap(PlayerSetup setup, GameStateMode mode);
// C.O.A x Cyb

    bool               ok() const { return ok_; }
    const std::string& error() const { return error_; }
    const PlayerSetup& setup() const { return setup_; }
    GameStateMode      mode() const { return mode_; }

    // ---- the join sequence ------------------------------------------------------------------------------------------

    /// Every step below in order. nullopt (and the shadows as they were) when one fails. Call once.
    std::optional<std::vector<Send>> join_sequence();

    std::optional<Send> player_controller_open();
    std::optional<Send> hud();                    // ClientSetHUD
    std::optional<Send> voice_and_level_list();   // pkt#23 chSeq 958
    std::optional<Send> flush_bandwidth_and_time();   // pkt#23 chSeq 959
    std::optional<Send> game_state_open();
    std::optional<Send> load_tracker_before_possession();
    std::optional<Send> pawn_open();
    std::optional<Send> restart_camera_rotation();    // ClientRestart, ClientSetCameraMode, ClientSetRotation
    std::optional<Send> possession_update();      // the PlayerController after possession
    std::optional<Send> pawn_second_state();      // call after pawn_open()
    std::optional<Send> player_state_open();
    /// Plan D1: the PlayerController's XPTracker (ULevelingComponent), a stably named subobject with the setup's progress
    /// (ExperienceFastMap and ExperienceDebtFastMap's Adventuring items, SkillTreePurchasesFastMap's Universal entry when the
    /// level has granted a rank): a PlayerController update that adds the component, the join's last chain. The original
    /// sent it in pkt#103 with the possession update (8,051 bits of a level-20 tester's maps); it is its own chain here so
    /// that the PlayerController's pkt#103 golden holds.
    std::optional<Send> xp_tracker_open();

    // ---- replies ----------------------------------------------------------------------------------------------------

    /// The pawn's phase-in (the reply to ServerClientIsReadyToPhaseIn, pkt#2523): bHidden 0 and BaseCharacterInfo
    /// bIsPhasedOut 0, bClientIsReadyToPhaseIn 1. Call after pawn_second_state(). The original sent it unreliable;
    /// this is reliable, because a lost unreliable update would leave the pawn hidden for good (the shadow has moved on).
    std::optional<Send> phase_in();
    /// Plan E1, stage 3 fix: ClientSetTime alone, for `world_time` (the server's clock at the send): the client's synchronized clock is set to it and
    /// ticks on from there. Sent again after the phase-in and every 120 s, as retail repeats it (pkt#23 and every 120.0 s after).
    std::optional<Send> set_time(double world_time);

    // ---- Nodes 1, stage 2: the nodes ---------------------------------------------------------------------------------

    /// Brings the GameState's node maps to `maps` (the world's node levels changed) and builds the update the client gets: only what differs from what the
    /// connection has goes out, as a fast-array delta on the GameState's channel.  nullopt when the mode has no node maps, the schema refuses them, or nothing
    /// changed (the arrays still hold what was last sent).  A reply that is not sent is built again from the checkpoint.
    std::optional<Send> node_maps_update(const repl::NodeStateMaps& maps);
    /// ClientReceiveNodeSummary / ClientReceiveSiegeSummary / ClientReceiveAllCurrentNodeSiegeCooldowns on the PlayerController (reliable): the answers to the
    /// client's 49 + 49 login requests and to the node window's.  nullopt before the pawn's second state (the client's world exists only then) or when the RPC does
    /// not build.
    std::optional<Send> node_summary(const repl::NodeSummary& summary);
    std::optional<Send> siege_summary(const repl::SiegeSummary& summary);
    std::optional<Send> siege_cooldowns();

    /// The reply to ServerUpdateAccountCombatSettings (pkt#620): the PlayerController's
    /// CharacterInGameSettings.CombatSettings.* properties the caller extracted from the client's request, as an update
    /// (nothing when none changed). Reliable, as phase_in().
    std::optional<Send> combat_settings_echo(std::span<const PropertyValue> values);

    /// ClientReceiveStoragePayload of an empty storage, on the pawn (the first state, and the reply to
    /// ServerRequestStorageData) or on the PlayerController (the reply to ServerRequestClientLoad). `target` is
    /// Role::Pawn or Role::PlayerController. The component's export goes along until it is acknowledged.
    std::optional<Send> storage_payload(StorageKind kind, Role target);

    // ---- plan B2: items --------------------------------------------------------------------------------------------------

    /// The PlayerController's ClientReceiveStorageEvent calls for `events`, one RPC each, in order, in one chain (reliable): at most
    /// kMaxEventsPerChain of them, none for an empty list. Each event names the storage's component, the slot, the item and its guid
    /// and the character as instigator when the event says so (ItemEvent::instigated). The bootstrap keeps its own copy of what each
    /// storage holds (the payloads' source) and each event changes it, so a payload built later has the items as the client was told
    /// of them. Applying an event twice (a chain refused and built again) changes nothing the second time.
    std::optional<Send> item_events(std::span<const ItemEvent> events);
    /// Plan E1, stage 3, part 2: a corpse's loot window.  ClientReceiveLootStoragePayload(LootActor = the corpse, its stacks as the window shows them).
    std::optional<Send> loot_payload(const emit::FIntrepidNetworkGUID& loot_actor, const repl::StoragePayload& data);
    /// The stacks the player took out of the loot container `container`: a type-1 ClientReceiveStorageEvent for each (retail's order: the removal from the
    /// container, then the add to the bag).
    std::optional<Send> loot_removed(const emit::FIntrepidNetworkGUID& container, std::span<const LootRemoval> removed);
    /// ClientRemoveListenerFromStorage(LootActor): the window closes (retail: when the corpse holds nothing more).
    std::optional<Send> loot_listener_removed(const emit::FIntrepidNetworkGUID& loot_actor);

    // ---- plan E1, stage 3, part 2: the player's death (research/combat 5)
    /// The pawn's own CombatInfo.bIsDead (sent with the next pawn update).
    bool set_dead(bool dead);
    /// The pawn's ClientSetLastDeathLocation(Location).
    std::optional<Send> death_location(double x, double y, double z);
    /// The PlayerController's ClientRestart(NewPawn) and ClientSetCameraMode, twice, as the death frame of the retail captures has them.
    std::optional<Send> death_restart();
    /// The pawn's SendClientRespawnChoices(RespawnChoices {ClosestLegalRespawns, HomeRespawn}): where the player may come back.
    std::optional<Send> respawn_choices(std::span<const int64_t> closest, int64_t home, bool previous_choice_invalid = false);

    /// Plan E1: ClientOpenVendor on the PlayerController (reliable): the client opens its window for the VendorDef `merch_set` (the vendor's
    /// stock and prices are the client's own tables'), `vendor` the NPC's actor. nullopt before the pawn's second state or when the RPC does
    /// not build.
    std::optional<Send> open_vendor(int64_t merch_set, const emit::FIntrepidNetworkGUID& vendor);
    /// ClientSetNodeTaxTable: the answer to the vendor window's ServerGetNodeTaxTable (fix 4; vendor/node_tax.h).
    std::optional<Send> node_tax_table(const repl::NodeTaxDetails& details);

    /// The join's events of one storage: an Add for each item the bootstrap holds in it, by slot, in chains of kMaxEventsPerChain.
    /// nullopt when an event does not build; an empty list for a storage with no items.
    std::optional<std::vector<Send>> storage_adds(ItemStorage storage);

    /// Sets what the pawn shows (plan B2): the HeldItems fast array (`held`, each entry's ReplicationID, slot and record; entries are
    /// the 23 slots) and the AppearanceIDs array. With HeldItems empty so far its items are added, otherwise every one is changed, marked
    /// dirty as the retail server does: once for each change of the Equipment storage an answer holds (an equip is one, a two-hander over a main
    /// hand and an off hand three: the capture's array key moved by 23 and by 69), `marks` times; AppearanceIDs changes only when its content
    /// does. Nothing goes out until equipment_update(). False, with the state as it was where it can be, when the pawn's second state is not
    /// built yet or a value does not fit.
    bool apply_equipment(std::span<const HeldItem> held, std::span<const repl::AppearanceId> appearance, size_t marks = 1);

    /// What changed on the pawn since the last update (HeldItems, AppearanceIDs, whatever else is dirty), as one chain, reliable; nullopt when
    /// nothing did. Take a checkpoint() first, as for every send.
    std::optional<Send> equipment_update();
// (C.O.A) (C-Y-B)

    /// How many items the bootstrap holds in a storage (its copy), and the item in a slot (nullptr when empty).
    size_t item_count(ItemStorage storage) const;
    const repl::ItemInstance* item_at(ItemStorage storage, int32_t slot) const;

    /// ClientUpdateGroup of an empty group, and the class's default action bar (ClientReceiveActionbarSlotPayload,
    /// as captured): the replies to ServerRequestClientLoad.
    std::optional<Send> empty_group();
    std::optional<Send> default_action_bar();
    /// Plan D1, stage 2: the character's own action bar as one ClientReceiveActionbarSlotPayload on the PlayerController (reliable): the reply to
    /// ServerRequestClientLoad, and the clearing of a class's slots when the class changes.
    std::optional<Send> action_bar(const repl::ActionbarPayload& payload);

    // ---- the mounts milestone, stage 2 -------------------------------------------------------------------------------------------------
    /// What retail sends the PlayerController when a mount is summoned (and when it is ridden): ClientClearActionbarsForInstigators([Vehicle, Mount],
    /// bank 18) and ClientReceiveActionbarSlotPayload(bank 18, `slot`: the mount's dash ability on the Mount instigator), one chain [R: captures 911,
    /// 896, 898, 871; the clear first].
    std::optional<Send> mount_bar(int64_t dash_ability, int32_t slot = 0);
    /// A callout of the PlayerController's CalloutQueueReplication: one item (Message `message`, SetCalloutType `type`, PointSize `point_size`, the
    /// creation time in .NET ticks) under the key `key`, as the retail server shows "Cannot summon mount here." [R: 876, 870]. Idempotent for a key that
    /// is added already (a chain built again after a checkpoint restore). `key` is the caller's: 0 is refused.
    std::optional<Send> callout_add(const std::string& message, int32_t type, int32_t point_size, int64_t creation_ticks, uint64_t key);
    /// Takes the item of `key` off the array (retail does, three seconds after it was shown). Idempotent; nullopt for a key never added.
    std::optional<Send> callout_remove(uint64_t key);
    /// The callouts the array holds (tests).
    size_t callouts() const { return callout_ids_.size(); }

    /// The acknowledgement of a ServerMovePacked: ClientMoveResponsePacked with FCharacterMoveResponsePackedBits' header
    /// for "good move" (10 bits, 645) and the client's TimeStamp (a float), an unreliable bunch of 68 bits on the pawn's
    /// channel, as the original's 4,191 (pkt#1900 on) and the legacy server's ClientAckGoodMove (src/net/game_server.h
    /// send_client_ack_good_move; the wire field is ClientMoveResponsePacked, field 35). Touches no shadow.
    std::optional<Send> good_move_ack(float time_stamp) const;

    /// The correction of a move the checks refused (plan A6, spec section 10): ClientAdjustPosition on the pawn's channel,
    /// which puts the client back at a position the server holds. `time_stamp` is the TimeStamp of the refused move (the
    /// client finds that saved move, takes the position and replays the moves after it), `location` the last good position
    /// in cm, `velocity` in cm/s (worldd does not simulate: zero), `movement_mode` the packed movement mode of the last good
    /// move. NewBase is none, NewBaseBoneName None, bHasBase and bBaseRelativePosition false. Unreliable: the function's
    /// flags (0x01820c42) have no FUNC_NetReliable, and the next refused move brings the next correction. Touches no shadow.
    std::optional<Send> correction(float time_stamp, const repl::Vec3d& location, const repl::Vec3d& velocity, uint8_t movement_mode) const;

    // ---- the mounts milestone, stage 3: riding (bootstrap/mount_ride.cpp) ---------------------------------------------------------------------------
    /// The acknowledgement of a ServerMovePacked on the channel of `actor`: a possessed mount (an ANPCCharacter) answers its rider's moves with the same
    /// ClientMoveResponsePacked as the pawn does [R: 910, 959, 871: a 56-bit block on the mount's channel per move]. Unreliable. The caller sends it on the
    /// actor's channel (Send::channel). Touches no shadow.
    static std::optional<Send> move_ack_on(const repl::Actor& actor, float time_stamp);
    /// The correction of a refused move on the channel of `actor` (ClientAdjustPosition, unreliable; Send::channel is the caller's). No capture shows the
    /// retail server correcting a mount [R: none in 910, 959, 871]: the pawn's rule, on the mount.
    std::optional<Send> correction_on(const repl::Actor& actor, float time_stamp, const repl::Vec3d& location, const repl::Vec3d& velocity,
                                      uint8_t movement_mode) const;
    /// ClientEndAbility(ClientInstanceId) on the AbilityComponent `ability_component` of `actor` (a possessed mount: its rider's cast of the dash was refused).
    /// Reliable; the caller gives it the actor's channel (Send::channel).
    std::optional<Send> end_ability_on(const repl::Actor& actor, const emit::FIntrepidNetworkGUID& ability_component, int32_t client_instance_id) const;
    /// The mount's InteractableComponent tells the client to play the interaction (the rider's mounting animation): ClientSideHandleInteractionAnimation(
    /// InteractingCharacter = the rider's pawn, InputIndex) on the component of `actor` named `interactable` [R: 910 packet 306, 70 ms after the request, with the
    /// PlayerController's ClientRestart; unreliable: the function's flags have no FUNC_NetReliable]. The caller gives it the actor's channel (Send::channel).
    std::optional<Send> interaction_animation_on(const repl::Actor& actor, const emit::FIntrepidNetworkGUID& interactable, uint16_t input_index) const;
    /// Getting on a mount, the PlayerController's first packet [R: 910 pkt 306, 871, 890, 895]: ClientRestart(NewPawn = the mount), ClientSetCameraMode
    /// (EName 204), ClientClearActionbarsForInstigators([Vehicle, Mount], bank 18) and the bank-18 slot of the mount's dash. One reliable chain.
    std::optional<Send> possess_mount(const emit::FIntrepidNetworkGUID& mount, int64_t dash_ability, int32_t dash_slot);
    /// ClientRestart(NewPawn = the player's own pawn) and ClientSetCameraMode, `times` times in one chain [R: 912 pkt 139 once; 893 pkt 142 twice].
    std::optional<Send> restart_own_pawn(int times = 1);
    /// The PlayerController's second packet [R: 910 pkt 310, 912 pkt 139]: Pawn and the attachment to `actor` (the mount, or the own pawn again), whose
    /// CollisionCylinder `cylinder` is exported with it. As the join's possession update (possession_update) does for the own pawn.
    std::optional<Send> controller_attach(const emit::FIntrepidNetworkGUID& actor, const emit::FIntrepidNetworkGUID& cylinder);
    /// The pawn's InteractInfo (UNPCSecondaryInfo "InteractInfo", the player's own default component that holds MountPtr) is a subobject the join did not send
    /// [R: 910 pkt 310 names it; 1198's pawn open lists it]: added once, named by `guid`. False when it exists with another guid or the schema is missing.
    bool ensure_interact_info(const emit::FIntrepidNetworkGUID& guid);
    /// The summoned mount the player can mount [R: 871 packet 210, 898 packet 158, 1198 packet 1706]: the pawn's InteractInfo.MountablePet (UCharacterSecondaryInfo, RepNotify)
    /// = `mount`, or 0 (a default GUID) once the mount has closed [R: 898 packet 601].  The client's action bar resolves a slot whose Instigator is Mount through it (exe:
    /// UseActionbarSlot 0x6b12700 and CanEverUseActionbarSlot 0x6a91a10 take GetActionbarInstigator 0x6aba180 -> the player character's GetMountablePet): without it the
    /// mount's dash (bank 18) cannot be used, nothing is sent and nothing is said (stage 3 fix 3).  The InteractInfo must exist (ensure_interact_info).  One pawn update.
    std::optional<Send> pawn_mountable_pet(const emit::FIntrepidNetworkGUID& mount);
    /// Getting on [R: 910 pkt 310]: the rider's pawn is attached to `mount` at MountSocket (AttachComponent = its CharacterMesh0 `mesh`, exported with the chain
    /// as the mount's component), RemoteRole 1, no PlayerState and no Controller, the attachment state (zone 2, lerp, valid), InteractInfo.MountPtr = the mount
    /// and, with the caller's set_abilities, the AbilityComponent's lists. `scale_percent` is the attachment's RelativeScale3D in percent. One reliable chain
    /// of the pawn's changed blocks.
    std::optional<Send> pawn_mounted(const emit::FIntrepidNetworkGUID& mount, const emit::FIntrepidNetworkGUID& mesh, int64_t scale_percent = 100);
    /// Getting off [R: 912 pkt 139]: RemoteRole 2, no attachment, PlayerState and Controller back, the attachment state cleared, RepDetachmentLocation (cm) and
    /// RepDetachmentRotation (the yaw), MountPtr cleared. One reliable chain.
    std::optional<Send> pawn_dismounted(const repl::Vec3d& detach_location, double yaw_degrees);
    /// Whether the pawn is attached to a mount as far as the bootstrap's state goes (tests).
    bool pawn_is_mounted() const { return pawn_mounted_; }
// ~ CoA & C:Y:B ~

    // ---- plan D1: the level, the XP and what a level grants ---------------------------------------------------------------

    /// Sets the XPTracker's Adventuring XP and debt (absolute totals, as retail replicates them). Nothing goes out until leveling_update().
    /// False, nothing changed, when a value does not fit or the tracker is not built yet.
    bool set_progress(int64_t xp, int64_t debt);
    /// Plan D1, stage 2: the rank list of one skill tree (the whole list, as retail sends it each time the tree changes; no ranks erase the
    /// tree's item). Every tree's key is the tree's Guid and TypeId with the hardcoded name 0, as the retail server's replies key it. Nothing
    /// goes out until leveling_update(). False, nothing changed, when the list does not fit.
    bool set_tree_ranks(int64_t tree, const std::vector<int64_t>& ranks);
    /// Marks a tree's item dirty without changing it, so that the next update carries the whole list again (a refused request: the client
    /// is corrected). False when the tree has no item.
    bool resend_tree(int64_t tree);
    /// The ranks the XPTracker lists for a tree as the client was told (empty when it has none).
    std::vector<int64_t> tree_ranks(int64_t tree) const;
    /// What changed on the XPTracker since the last update, as one PlayerController chain, reliable; nullopt when nothing did.
    std::optional<Send> leveling_update();
    /// The pawn's BaseCharacterInfo.Level (the UPlayerInfo property the client shows) and CombatInfo.PrimaryClassId.Guid: set
    /// here, sent with the next pawn update (pawn_state_update(), or the equip reply's). False when the value is refused.
    bool set_level(int level);
    bool set_class(int64_t class_record_guid);
    /// Plan E1, stage 3: the pawn's CombatInfo.bInCombat (the first exchange of a fight sets it, six seconds without one clears it). The caller sends the
    /// pawn's update (pawn_state_update).
    bool set_in_combat(bool in_combat);
    /// Plan E1, stage 3 fix: the pawn's CombatInfo.bIsUsingRangedWeapon (ServerSetUseRangedWeapon's answer in retail: the client's weapon in hand follows
    /// it). The caller sends the pawn's update.
    bool set_using_ranged_weapon(bool ranged);
    /// Plan D1, stage 2: OwnerInfo.WeaponSkillLineId (the weapon line of the weapon in hand), sent with the next pawn update.
    bool set_weapon_skill_line(int64_t guid, const std::string& name);
    /// Marks the TempHealth float-array item dirty, as every captured level-up's packet has it (the same item, no value):
    /// the next pawn update carries it. False when the pawn has no such item.
    bool mark_temp_health();
    /// Whatever is dirty on the pawn (the level, the class, the stats, the TempHealth mark, HeldItems ...) as one chain,
    /// reliable; nullopt when nothing is. Take a checkpoint() first, as for every send.
    std::optional<Send> pawn_state_update(const char* what = "level");
    /// BroadcastPlayLevelUpEffects(Category, InLevel) on the pawn: the level-up's effects and sound (unreliable, as the
    /// function's flags say: a lost one costs the fanfare, not the level). `category_guid` is the LevelingTypeDef (Adventuring).
    std::optional<Send> level_up_broadcast(int64_t category_guid, int level);
    /// ClientUpdateSkillOnActionbar on the PlayerController: the client places an auto-granted skill (bUseHint, as retail's
    /// auto-grant and every purchase reply). Reliable.
    std::optional<Send> skill_on_actionbar(const repl::SkillOnActionbar& update);
    const repl::LevelingState& leveling() const { return leveling_; }

    // ---- plan A7: abilities and stats after the join -----------------------------------------------------------------

    /// Applies what the ability runtime changed (ability_replication.h) to the pawn's replicated state: the Owner,
    /// OwnerProxy and EveryoneProxy stat items and CurrentEffects of the StatsComponent, ReplicatedActiveAbilities and
    /// Cooldowns of the AbilityComponent. Nothing goes out until ability_update(). False, with the state as it was, when the
    /// pawn's second state is not built yet, a stat has no item, a key to change or remove is unknown or a key to add is
    /// taken (the runtime's bookkeeping and the replicated arrays disagree: a bug, not something a client can cause).
    bool apply_ability_change(const AbilityChange& change);

    /// Plan D1, stage 2: sets the AbilityComponent's KnownAbilities and ReplicatedRecordsToLoad (whole lists: the next update carries the
    /// elements that differ from what the connection has, from the first changed index on, as the retail server's appends, removals and
    /// replacements are). Nothing goes out until ability_update(). False when the pawn's second state is not built yet or a list is too long.
    bool set_abilities(std::span<const int64_t> known, std::span<const AbilityRecord> records);
// C-O-A with Cyb

    /// Plan B3: the values of the three Int32 stat arrays as the pawn holds them now (the join's and every change since), by stat
    /// Guid. Empty before the pawn's second state is built.
    std::map<int64_t, float> current_stats();
    /// Plan D1: whether the three Int32 stat arrays were laid out with an item for the stat at the join (a class change brings stats the
    /// arrays were not laid out for: the new class's own resource, which has no item until the next login). It is the layout at the join,
    /// not what the arrays hold now: an item an array lost later is a change the pawn refuses, not a stat that was never there.
    bool has_stat_item(int64_t record) const;

    /// What changed on the pawn since the last update, as one chain, reliable (the original's updates were unreliable; a lost
    /// one would leave the shadow ahead of the client, as phase_in()): the AbilityComponent's block then the StatsComponent's,
    /// as pkt#8240 has them, with the export of a montage the client has not acknowledged. nullopt when nothing changed. Call
    /// it once the join is on the wire (the second state is built then) and take a checkpoint() first, as for every send.
    /// `failed`, when given, says which: set when the update does not build (a block that cannot be written, a chain that does not
    /// assemble: error() then says why), false when nothing changed.
    std::optional<Send> ability_update(bool* failed = nullptr);

    /// How many ability items, effect items and cooldown items the pawn lists now (what apply_ability_change has added and not
    /// removed): what a client keeps when no more updates are sent.
    size_t listed_abilities() const { return ability_ids_.size(); }
    size_t listed_effects() const { return effect_ids_.size(); }
    size_t listed_cooldowns() const { return cooldown_ids_.size(); }

    /// ClientEndAbility(ClientInstanceId): the server ending an instance on the client (a refusal, or a sprint that ran
    /// out of Stamina). Reliable, on the pawn's channel, after whatever update removed the instance's items.
    std::optional<Send> end_ability(int32_t client_instance_id);

    /// Plan D2, stage 2: ClientHitConfirmationsReliable on the PlayerController: the hits of `confirms`, each one on the pawn (HitComp, Instigator
    /// and, for an ability's or an effect's hit, Source are its guid) with the stat mods the client draws its floating numbers from. `at` is the
    /// impact location (the pawn's position, cm). The InstGuid of each hit is the bootstrap's own counter. nullopt for an empty list or a call that
    /// does not build.
    std::optional<Send> hit_confirmations(const std::vector<HitConfirm>& confirms, const repl::Vec3d& at);
    /// E1 stage 3 fix 2: the export chain of another actor's collision cylinder (`component`, owned by `actor`): what the ability update and the hit
    /// confirmations carry so the client can resolve the Targets and HitComps that name an NPC's cylinder [R: retail's "CollisionCylinder" export, flags 7,
    /// checksum 0xb876932f, the NPC actor as the outer].
    static repl::ExportChain npc_component_export(const emit::FIntrepidNetworkGUID& actor, const emit::FIntrepidNetworkGUID& component);

    // ---- plan D2, review: lingering zones' actors --------------------------------------------------------------------------

    /// Builds the actor of a lingering zone (nothing goes out yet). False when a value does not fit or the actor's id is taken.
    bool lingering_add(const LingeringSpec& spec);
    /// Plan E1: the same actor as a value, for the world's registry (the session hands a zone's actor to the actor system instead of opening it itself):
    /// nullopt when a value does not fit or the pawn's second state is not built. Its AttachComponent names the pawn's CharacterMesh0, a default
    /// component the client finds by name and outer: `lingering_mesh_export` is the export that goes with the actor's open.
    std::optional<repl::Actor> lingering_actor_of(const LingeringSpec& spec);
    repl::ExportChain          lingering_mesh_export(const emit::FIntrepidNetworkGUID& pawn_mesh) const;
    /// The open of the actor's channel (reliable, Send::effect_actor = its ObjectId). nullopt for an actor that is not added or does not build.
    std::optional<Send> lingering_open(uint64_t actor_id);
    /// bFadingOut = 1, as the retail server sends it 0.6 s before the actor is destroyed (one update, reliable).
    std::optional<Send> lingering_fade(uint64_t actor_id);
    /// The actor's channel was closed: the bootstrap forgets it.
    void lingering_remove(uint64_t actor_id);
    size_t lingering_count() const { return effect_actors_.size(); }
    /// The actor as the bootstrap holds it (for tests); null when there is none.
    const repl::Actor* lingering_actor(uint64_t actor_id) const;

    // ---- acknowledgements and atomicity ---------------------------------------------------------------------------

    /// The GUIDs whose export the client acknowledged (Emission::exported_by_bunch[i] of a delivered export part).
    void mark_exported(std::span<const emit::FIntrepidNetworkGUID> guids) { acks_.mark(guids); }
    repl::ExportAcks&       export_acks() { return acks_; }
    const repl::ExportAcks& export_acks() const { return acks_; }

    /// What the connection has of the four actors, and (plan B2) the pawn's HeldItems array. Restore it when a built Send was not
    /// accepted. apply_equipment() changes the array (its key moves with every change) before the update is built, so a chain the
    /// connection did not take must put the array back too, or the retry of the same reply moves the key a second time. (A checkpoint taken
    /// before the pawn's second state is added, as join_sequence's is, leaves the arrays alone: that state is added once.)
    struct Checkpoint {
        repl::ActorShadow player_controller, game_state, pawn, player_state;
        bool              second_state_sent = false;
        std::optional<repl::FastArrays> base_arrays;   // BaseCharacterInfo's fast arrays (HeldItems), once the second state is added
        std::optional<repl::LevelingState> leveling;   // plan D1: the XPTracker's maps, once the tracker is added (set_progress moves their keys)
        std::map<uint64_t, repl::ActorShadow> effect_shadows;   // plan D2: what the connection has of each lingering zone's actor
    };
    /// (Nodes 1, stage 2: node_maps_update undoes its own change to the GameState's node maps, which restore() puts back for the update built right after the checkpoint.
    /// checkpoint() forgets the one before; it does not copy the node maps' arrays, which a reply step that is not a node update never changes.)
    Checkpoint checkpoint();
    void       restore(const Checkpoint& c);

    // ---- the actors, for tests and for E3's later updates ----------------------------------------------------------

    const repl::Actor& player_controller() const { return pc_; }
    const repl::Actor& game_state() const { return gs_; }
    const repl::Actor& pawn() const { return pawn_; }
    const repl::Actor& player_state() const { return ps_; }

private:
    bool build_actors();
    void add_second_state_components();
    std::optional<Send> finish(Role role, bool opens, const char* what, std::optional<repl::Emission> e) const;
    /// The two RPC requests of the mount bar (the bank-18 clear and the dash slot), appended to `calls`.
    bool mount_bar_calls(int64_t dash_ability, int32_t slot, std::vector<repl::RpcRequest>& calls) const;
    /// Plan D1: copies the LevelingState's arrays into the XPTracker subobject (the update is built from the subobject).
    void sync_xp_tracker();
    std::optional<Send> update(Role role, const char* what, repl::Actor& actor, repl::ActorShadow& shadow);
    std::optional<Send> rpcs(Role role, const char* what, std::span<const repl::RpcRequest> calls);
    std::optional<Send> verbatim_block(Role role, const char* what, const char* hex, size_t bits);
    repl::Subobject* component(const emit::FIntrepidNetworkGUID& guid) { return pawn_.find(guid); }
    const emit::FIntrepidNetworkGUID& storage_guid(StorageKind kind) const;
    std::optional<repl::Emission> game_state_captured_open() const;
    bool fail(std::string message);

    PlayerSetup       setup_;
    GameStateMode     mode_;
    repl::Actor       pc_, gs_, pawn_, ps_;
    repl::ActorShadow pc_shadow_, gs_shadow_, pawn_shadow_, ps_shadow_;
    repl::ExportAcks  acks_;
    bool              ok_ = false;
    bool              second_state_added_ = false;   // the pawn has its second state's components and values
    bool              second_state_sent_ = false;    // and the update that carries them was built
    std::set<int64_t> stat_layout_;                  // the stats the Int32 stat arrays have an item for (as the second state laid them out)
    std::string       error_;
    // plan A7: the runtime's keys and the fast arrays' ReplicationIDs (ability_replication.h "Keys")
    std::map<int64_t, int32_t>     effect_ids_, ability_ids_, cooldown_ids_;
    bool                           pawn_mounted_ = false;   // the mounts milestone, stage 3: pawn_mounted() ran and pawn_dismounted() has not
    std::map<uint64_t, int32_t>    callout_ids_;       // the mounts milestone: the callouts keys and their ReplicationIDs
    std::set<uint64_t>             callouts_removed_;  // keys taken off (a chain built again after a restore only builds the update)
    uint32_t                       hit_instances_ = 0;   // plan D2, stage 2: the hits confirmed so far (the InstGuid counter)
    std::map<int64_t, std::pair<std::string, uint64_t>> ability_montages_;   // the montage path and static NetGUID (0: the table's) of each instance in ReplicatedActiveAbilities
    /// E1 stage 3 fix 2: the other actors' components each item's Targets name (actor, component), exported with the AbilityComponent's update as that
    /// actor's "CollisionCylinder" until the client acknowledges the export (ExportAcks), as retail does in the bunch that first names the cylinder
    std::map<int64_t, std::vector<std::pair<emit::FIntrepidNetworkGUID, emit::FIntrepidNetworkGUID>>> ability_targets_;
    // plan D2, review: the lingering zones' actors by ObjectId, and what the connection has of each
    struct EffectActor {
        repl::Actor       actor;
        repl::ActorShadow shadow;
    };
    std::map<uint64_t, EffectActor> effect_actors_;
    emit::FIntrepidNetworkGUID      mesh_;   // the pawn's CharacterMesh0 the lingering actors attach to (the session's one guid)
    // plan B2: what each storage holds as the client was told, by (storage, slot)
    std::map<std::pair<uint8_t, int32_t>, repl::ItemInstance> items_;
    // plan D1: the XPTracker's maps (the typed state keeps the keys the way MarkItemDirty does) and whether the subobject is on the actor
    repl::LevelingState leveling_;
    bool                xp_tracker_added_ = false;
    /// Nodes 1, stage 2: the GameState's node maps as they were before the last node_maps_update that built an update (it changes them before the chain exists); restore()
    /// puts them back, checkpoint() forgets them, so a reply step that is not a node update copies no array
    std::optional<repl::FastArrays> node_maps_before_;
    const emit::FIntrepidNetworkGUID& item_storage_guid(ItemStorage storage) const;
    std::optional<Send> build_item_events(std::span<const ItemEvent> events);
};

}  // namespace aoc::worldd::bootstrap
