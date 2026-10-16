// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_session.h
//
//  One joined player on worldd (plan A5, spec section 4): what happens on a
//  Connection after NMT_Join.  The Endpoint starts a PlayerSession when the
//  client's Join arrives and calls it for every tick, every bunch the client
//  sends on an actor channel and the acknowledgements the Connection reports.
//
//  Join.  The player's PlayerSetup is built from the character record, content.db,
//  the login identity and the session's values (bootstrap/character_setup.h); a
//  PlayerBootstrap makes the sequence (bootstrap/bootstrap.h join_sequence(): the
//  PlayerController, GameState, pawn and PlayerState opens and the RPCs between
//  them, the original's order).  Each actor gets a channel: the lowest index from 3
//  that is neither open, nor closing on the Connection, nor taken by an earlier
//  role of this player (the original's were 3, 6, 19 and 193; a fresh Connection
//  gives 3, 4, 5 and 6).
//
//  Sending.  Every Send goes through Connection::send_chain, in order, one at a
//  time.  The bootstrap advanced its replication state when it built a Send, so a
//  Send that cannot go out yet (can_send_chain: a channel's window of 256
//  unacknowledged reliable bunches is full) stays at the head of the queue and is
//  tried again once an acknowledgement has freed some of that channel's window (until
//  then a tick does nothing), and nothing behind it overtakes it: the order of the
//  original's sequence is the order of the shadows.  A reliable chain that is
//  refused with nothing in flight on its channel can never be sent; for the join
//  that fails the session (the Endpoint then refuses the login with
//  login_text::kLoginFailed), for a reply it is dropped and logged.
//
//  Replies.  The client's requests become reply steps built when they are about to
//  go out, after the join is on the wire: take a checkpoint, build, send_chain,
//  and restore the checkpoint when the chain cannot go yet (the contract of
//  bootstrap::PlayerBootstrap::checkpoint and the replicator's "emit, then commit
//  only when the chain was accepted"); the same step is built again when the
//  window has room.  At most kMaxPendingReplies steps wait; a request that does not
//  fit whole (the six of ServerRequestClientLoad) is ignored, and logged once.  The
//  move acknowledgement is unreliable and goes out at once or not at all, like every
//  unreliable bunch.
//
//  Acknowledgements.  Connection::take_ack_events reports each part of each chain
//  when its packet is acked or lost.  An export part that was delivered marks its
//  GUIDs exported (PlayerBootstrap::mark_exported), so later chains name those
//  objects without their paths; a lost reliable part is sent again by the
//  Connection and its event follows, a lost unreliable one is final.
//
//  The client's bunches (the class schema of the channel's actor, repl::
//  read_client_bunch), by RPC name:
//
//    PlayerController   ServerAcknowledgePossession          logged
//                       ServerRequestClientLoad              the four storage payloads on the PlayerController,
//                                                            ClientUpdateGroup (empty) and the class's default
//                                                            action bar (transcript section 2.2, pkt#597-607)
//                       ServerUpdateAccountCombatSettings    the nine CombatSettings values of pkt#620 as the
//                                                            client's own, on the PlayerController's
//                                                            CharacterInGameSettings (below)
//    pawn               ServerClientIsReadyToPhaseIn         the phase-in update, once (pkt#2523)
//                       ServerRequestStorageData             the storage payload of the container named
//                                                            (pkt#773)
//                       ServerMovePacked                     checked (below); a plausible move gets
//                                                            ClientMoveResponsePacked, unreliable, with its
//                                                            TimeStamp, an implausible one ClientAdjustPosition
//    AbilityComponent   ServerTriggerAbility                 Sprint, Dodge and Rest start (plan A7, below); anything
//                                                            else, and a refused one, is answered with ClientEndAbility
//                       ServerNotifyHeldAbilityCancelled     releases a running Sprint or Rest
//    pawn               ServerItemUsed                       an item is worn or taken off (plan B2, below), or, for a bag item that is worn in
//                                                            no slot, used (plan D1, below)
//                       ServerActivateItem                   not answered: the client sends it with ServerItemUsed, the same four
//                                                            parameters, for an item that can be worn, and the server's answer is one
//    PlayerController   ServerRequestStorageEvent            a stack moved or swapped inside the Backpack (plan B2, below)
//    anything else      logged once per name, at debug, and ignored
//
//  Items (plan B2, items/inventory.h has the rules and what the retail server does).  The session plays the character's Inventory, built
//  at the join from the record's items.  ServerItemUsed names a storage (the Backpack 3 or the Equipment 4), a slot, the instance guid it
//  expects there and, for a bag item, the slot to wear it in (0xFFFFFFFF: the server picks): the Inventory applies it or refuses it
//  (changing nothing), and what the client is told is queued as reply steps, in the retail order and one packet: the PlayerController's
//  ClientReceiveStorageEvent calls (two for a plain equip or unequip, up to six for a two-hander over a main hand and an off hand) and,
//  for the Equipment, the pawn's update with HeldItems (all 23 items marked dirty, as the retail server's key moves by 23) and the
//  AppearanceIDs that changed.  The derived stats an equip changes are plan B3's: none are sent in B2 (the client's stats stay what
//  the join sent).  A refused request changes nothing and the client is corrected the way it is told its inventory at the join: the
//  Backpack's and the Equipment's ClientReceiveStoragePayload on the pawn, under a budget of its own.  ServerRequestStorageEvent is
//  read whole (a Move or a Swap of whole stacks between two slots of the Backpack, its source and target named by the pawn's storage
//  component, a guid that matches the item in the slot) and answered as the retail server does (a Move: Remove, Add; a Swap: Remove,
//  Remove, Add, Add); splitting, stacking, destroying and every other storage are refused.
//
//  Stats (plan B3).  The character's stats are the stat engine's (stats/character_stats.h): computed at the join from its class, level and
//  worn items (they fill the StatsComponent's arrays), and recomputed whenever the Equipment changes.  What changed (the stats whose float
//  bits differ; a current Health or Mana above its new maximum is clamped to it) is applied to the pawn at once with the equip and goes
//  out in the same packet as the HeldItems and AppearanceIDs update: only the changed stats, as deltas of the arrays they are in.  A
//  stat the ability runtime holds (MaxStamina, the regeneration and MoveSpeedMult an effect raises) goes through it, so a running Rest or
//  Sprint keeps its effect on the new value, and a smaller MaxStamina clamps Stamina; the runtime starts from the pawn's values as they
//  are then, not the join's.  Nothing is sent when no stat changes.
//
//  Using an item (plan D1, stage 1; session/item_use.h and session/leveling.h have the rules).  A ServerItemUsed on a bag item that
//  content.db lists no slot for runs the item's action responses: a GiveItem's rewards go into the bag (fresh instances, an Add
//  event each, after the Remove of the consumed stack, or its Remove and Add with one less) and its Adventuring XP into the
//  character's progress; a SetPrimaryArchetype changes the class.  XP is replicated as the XPTracker's absolute totals (the
//  ExperienceFastMap and ExperienceDebtFastMap items of Adventuring, a PlayerController update).  A level crossed recomputes
//  the stats at the new level with the constant stat mods of the ranks the level grants (B3's equip-diff path: only the changed
//  stats go out), and sends what the captured level-ups had: BroadcastPlayLevelUpEffects on the pawn, the pawn's update with
//  BaseCharacterInfo.Level, the TempHealth float-array item marked, the stat deltas and an infinite CurrentEffects item per
//  granted passive, then the tree's rank list in the XPTracker and ClientUpdateSkillOnActionbar per granted skill (the
//  auto-grant as retail sent it for a weapon line).  The level is held at Adventuring's cap (25), the XP just below the next
//  threshold.  A class change recomputes the stats for the new class and sets CombatInfo.PrimaryClassId; stage 2 switches the
//  abilities, the tree and the bar live (below).  The progress (level, class, XP, debt, and since stage 2 the ranks and the bar) is
//  handed to the sink at once (SaveProgress) and again when the session ends if it changed at all.  A refused use changes nothing and is answered like a refused equip
//  (the storages' payloads, under the resync budget); a use that is not stage 1's (another response type, an unmodelled reward
//  table, a Weapon XP reward) is refused or noted and logged.  A use takes a token of the item budget, as an equip does.
//
//  Talents and the action bar (plan D1, stage 2; session/talents.h and session/action_bar.h have the rules and what is modelled).
//    pawn          ServerPurchaseSkillBuild      a purchase, a refund, a swap or a respec of ranks of one tree.  It is judged whole (points, level,
//                                                prerequisites, mutually exclusive ranks, no refund of a rank a level grants, no duplicates, at most
//                                                kMaxBuildChanges entries, a request budget); a refused request changes nothing, is logged, and the
//                                                tree's unchanged rank list is sent again so that the client's view is corrected (retail has no
//                                                refusal to copy).  An applied one is answered as the retail server answers, in this order: the
//                                                pawn's update (the AbilityComponent's KnownAbilities and ReplicatedRecordsToLoad changed by what the
//                                                ranks grant or take, a passive's CurrentEffects item or its removal, the stats the rank's constant mods
//                                                change), the XPTracker's whole rank list of each tree touched, and a ClientUpdateSkillOnActionbar for
//                                                each bought rank whose node is put on the bar (not for a refund).  An effect item lasts the effect's own
//                                                duration (infinite when the record has none): a marker effect (0.1 s of a weapon rank, 1 s of a Universal
//                                                rank) is removed by the server when it ends.  A respec is the same request with every owned rank refunded
//                                                (free); the ranks a level grants stay.
//    pawn          ServerSetUseRangedWeapon      the combo of the weapon in the target slot moves to the end of KnownAbilities (and into the records), and
//                                                the pawn's weapon skill line follows; the ranks of a weapon tree apply while that line is in hand; the
//                                                pawn's CombatInfo.bIsUsingRangedWeapon follows the request (E1 stage 3 fix: retail's reply has it, and
//                                                the client's weapon in hand follows it).
//    PlayerController ServerSetActionbarSlotUseData  the slot is stored (no reply); a slot outside the banks 0-19 and slots 0-15, an enumeration the
//                                                client does not have or a full bar is refused and counted.
//    PlayerController ServerRequestClientLoad    the stored bar is sent once as one ClientReceiveActionbarSlotPayload.
//  A level crossed grants the ranks the Universal tree and the class tree give by themselves (the part-2 spec's "Settled in D1": the abilities join
//  KnownAbilities, the passives get their item, the skills with the hotbar flag are put on the bar and announced).  A class scroll switches the class
//  live: the old class's abilities leave KnownAbilities and the records and its slots leave the bar (the slots are sent cleared: a payload with UseType
//  None), the new class's tree is listed and its auto-granted ranks granted, the stats are the new class's (its own resource stat gets an item in the
//  arrays), and each class keeps its bought ranks under its own tree.  What still needs a relog is in the README.  Every 1.000 s on a clock the sessions
//  share, Health and Mana gain their regeneration stat (D2's runtime will absorb this basic tick; Stamina stays the ability runtime's).
//
//  Saving the inventory.  An applied change marks the inventory dirty.  It is handed to the PositionSink (worldd's CharacterSaver, a
//  worker thread, so never the network thread) every save_interval when dirty, and in finish(), whatever ended the session, when it
//  changed at all this session (the saver may have lost a hand-over; the latest wins, so one more is harmless).  A second login of a
//  character that is playing starts from the old session's inventory, as it starts from its position.
//
//  Abilities (plan A7, spec section 7).  The AbilityComponent's two RPCs go to the player's AbilityRuntime
//  (session/abilities.h), which keeps Stamina and the three abilities on a clock of its own that starts at the session's first
//  tick, and says what changed.  The runtime's changes are applied to the pawn's replicated state at once
//  (PlayerBootstrap::apply_ability_change) and go out as one update when the connection has room: the update is built from
//  the shadow, so changes that pile up while a window is full are one chain later (the state is the queue, which never
//  grows), then the ClientEndAbility calls, in order.  Nothing goes out before the join's chains have (the pawn's second state
//  comes first).  A trigger takes a token of ability_budget_ (kAbilityBurst at once, one more every kAbilityInterval): a client
//  that sends more is dropped and counted, like the other requests.  A trigger the codecs cannot read whole is answered
//  from its first two parameters (the ability and the instance id), which are read by hand.  The A6 moves tell the runtime
//  whether the player is moving: the input direction the client sends with each move, or a displacement faster than
//  kMovingSpeed.  A Dodge opens a window (AbilityRuntime::dash_allowed) in which the move checker allows the dash's speed.  The
//  Sprint's MoveSpeedMult is a replicated stat (1.2: the client moves at 600 cm/s times it); the log's "ground speed while
//  sprinting" line (first few at info, like the other client-driven lines) shows what speed the client really moves at.
//
//  Movement and saving (plan A6, spec section 10).  ServerMovePacked's one parameter is read whole
//  (protocol/repl/move_packed.h: the new move, and the pending and old moves that ride along) and each move is
//  judged by the player's MoveChecker (session/move_checker.h) against the last good position: finite numbers, inside
//  Verra, a rising TimeStamp, no more distance than the speed limits allow.  The older moves of a packet are judged
//  first (oldest first, those already seen skipped), then the new one, whose verdict is the reply: accepted, then
//  ClientMoveResponsePacked with its TimeStamp (as in A5), and the move is the player's last good position and yaw;
//  refused, then PlayerBootstrap::correction, ClientAdjustPosition with the refused move's TimeStamp and the last good
//  position, so that the client snaps back and the next move is measured from there.  Nobody is kicked.  The checker is
//  given the Endpoint's clock (the `now` of on_bunch): the TimeStamp is the client's own clock and is paced against
//  the server's (move_checker.h), so moves that arrive together share one `now` and one bank of time.  A parameter
//  that does not read is not acknowledged (the client resends).  A move whose TimeStamp does not rise (a duplicate or
//  a datagram that arrived out of order) is dropped without a word to the client (debug log, counted apart from the
//  refused moves): UE ignores such a move too.  At most one correction goes out per datagram, the latest refusal's
//  (the Endpoint calls end_of_datagram after the datagram's bunches; a move that is accepted later in the same datagram
//  cancels it: the client is where the server has it), and its movement mode is never 0 (walking when the last good
//  move had none).  The other ServerMove variants (ServerMove, ServerMoveDual ...) are the old, unpacked form; the
//  client never sends them and they are neither checked nor acknowledged: they change nothing.
//
//  The last good position and yaw are handed to the world's PositionSink (worldd's CharacterSaver: a worker thread, so
//  never the network thread) when they differ from what was loaded or last handed over: every save_interval (60 s,
//  checked by tick(conn, now)) when the location changed, and by finish(), which the Endpoint calls when the session ends
//  (the client quit or timed out, a kick, a login elsewhere, a broken protocol) and when worldd stops.  finish() compares
//  with the record the session started from, not with the last hand-over (the saver may have lost that one): it hands
//  over the last good position when its location or yaw differs from the record's, or when a hand-over was made before
//  (the player may be back where it started).  A session that never moved saves nothing.
//
//  ServerUpdateAccountCombatSettings.  The original answered with a PlayerController property block holding the nine
//  values that differ from the class defaults (pkt#620: eight equal the request's, PartyVFX does not).  The defaults are
//  not known to worldd and the other 41 values of the request were never sent in a block, so exactly these nine paths
//  are echoed, each with the request's value (an absent one is left out).  Not proven in game.
//
//  The world's other actors (plan E1, stage 1; world/actor_registry.h, session/actor_view.h, spawns/area_loader.h).  The session owns an
//  ActorView on the world's registry: once the join and the phase-in are on the wire it gives the view the player's last good position
//  every tick, and the view opens, updates and closes the NPCs and resource nodes around the player on channels of its own (the first
//  indexes are the player's four actors', and the zone actors' ones, which it never takes).  The ack events the session takes are forwarded
//  to it, so that exports are marked delivered.  The session's own part of the area is the GM vendor:
//
//    pawn          ServerInteractWithInteractableComponent    the component names an NPC of the registry; it must be one the client has open
//                                                             (the view's awake state), within kInteractRangeCm of the last good position and a GM
//                                                             vendor (vendor/gm_vendor.h): then ClientOpenVendor(its VendorDef, the NPC) is sent and
//                                                             that NPC is the player's open vendor.  Anything else is ignored and logged.
//    PlayerController ServerHandleVendorPurchase              the open vendor's cart (vendor/gm_vendor.h plan_purchase): every item one the def
//                                                             lists, a sane quantity, room in the bag for every stack; refused whole otherwise
//                                                             (the client's storages are sent again, as for a refused item request).  An applied one
//                                                             grants the stacks into the bag (items::Inventory::grant) and answers with the storage
//                                                             events (an Add each, in chains of kMaxEventsPerChain), as a use's reward does; nothing
//                                                             is charged.
//                  ServerGetNodeTaxTable                      the vendor window asks for its node's taxes as soon as ClientOpenVendor opens it and
//                                                             shows no listing, tax or funds until the answer arrives: with a vendor window open
//                                                             (a GM vendor within range), ClientSetNodeTaxTable(the node asked for, vendor/node_tax.h's
//                                                             untaxed table of 78 tags) is sent, at most kTaxBurst at once and one more every
//                                                             kTaxInterval; with none, ignored.  Every request is logged (the first ones at info).
//                  ServerHandleVendorTransaction              a sale or a buy-back: GM vendors buy nothing; ignored and logged once.
//  Both take tokens of the item budget (kVendorBurst, kVendorInterval).
//
//  Riding (the mounts milestone, stage 3; session/riding.h has the rules and what is modelled).  The session controls ONE actor at a time: the player's pawn or
//  the mount it rides.  Getting on is an interaction with the mount's InteractableComponent (the mount is the player's, summoned and out, and within kInteractRangeCm):
//  the PlayerController is restarted on the mount (the camera mode, the bank-18 bar), the mount's actor in the world's registry gets its Owner and its rider's stats (the
//  actor view sends them on the mount's channel), the rider's pawn is attached to it and the PlayerController follows, all in the same flush.  From then on
//
//    mount channel   ServerMovePacked          checked by the mount's own MoveChecker (made at every possession: the client's TimeStamp restarts near 0; its horizontal limit
//                                              is the Blueprint's speed times MoveSpeedMult times 1.5), acknowledged and corrected on the mount's channel; the mount follows
//                                              (the registry's position, the road under it) but its owner is sent no ReplicatedMovement of it
//                    ServerTriggerAbility      on the mount's AbilityComponent: the dash (MountAbility_Sprint: Cooldowns 30 s, the 5 s effect, MoveSpeedMult x1.35, the
//                                              hit confirmation), refused with ClientEndAbility on the mount's channel while it cools down; any other ability follows the
//                                              filter: a bUsableWhileMounted one is run, a bAutoDismount one dismounts the rider first, the rest is refused
//    pawn channel    ServerMovePacked          dropped (a rider moves its mount, not its pawn): counted
//    PlayerController ServerDismountInteraction  the rider's pawn is put beside the mount at saddle height (RepDetachmentLocation), the PlayerController is restarted on it
//                    ServerDesummonMount        a rider dismounts first (two ClientRestarts in the packet when the client asked for the dismount too, as capture 893)
//
//  Hits aimed at a mounted rider land on the MOUNT's Health (the rider's own is untouched); at 0 the mount dies: the rider is dismounted, the mount closes, and its item keeps
//  Health 0 and a lock (the item cannot be summoned until it ends).  The player's death dismounts and desummons.  The position that is saved, and that a second login of the
//  character starts from, is the place the rider would be put beside the mount: relog while mounted logs in dismounted at the mount's last good position.
//
//  Budgets.  A request that pulls data costs the client a few bits and worldd kilobytes (a load is six chains, about
//  7 KB), so each kind has a token bucket on the Endpoint's clock (the `now` of on_bunch; the session keeps no clock
//  of its own): kLoadBurst loads at once and one more every kLoadInterval (the combat-settings echo the same), and
//  kStorageBurst storage payloads with one more every kStorageInterval.  A request over its budget is dropped and
//  counted; the first drop is logged.  The budget never looks at what is unacknowledged: the retail client's first
//  load arrives while the join's chains are still in flight.
//
//  Logging.  What the client can cause per call (a possession acknowledgement, a load, the combat settings, the
//  "sent" line of a reply chain, a refused move, a periodic save) is at info the first kLoggedPerKind times and at
//  debug after, like the malformed bunches; the join's chains and the final save are always at info.
// ============================================================================
#pragma once
// [Cyb :: C | O | A]

#include "bootstrap/bootstrap.h"
#include "gather/gathering.h"
#include "items/inventory.h"
#include "net/connection.h"
#include "net/control_channel.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/node_rpc.h"
#include "protocol/repl/vendor_rpc.h"
#include "session/abilities.h"
#include "session/actor_view.h"
#include "session/ability_pipeline.h"
#include "session/action_bar.h"
#include "session/item_use.h"
#include "session/leveling.h"
#include "session/mounts.h"
#include "session/move_checker.h"
#include "session/player_world.h"
#include "session/riding.h"
#include "session/position_saver.h"
#include "session/professions.h"
#include "session/talents.h"
#include "stats/character_stats.h"
#include "svc/services.h"

#include <spdlog/common.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aoc { namespace worldd {

/// A token bucket on the caller's clock: `burst` takes at once, then one more every `per_token`.  Unused time past a
/// full bucket earns nothing.
class RequestBudget {
public:
    RequestBudget(uint32_t burst, Clock::duration per_token) : burst_(burst), per_token_(per_token), tokens_(burst) {}

    /// Takes a token when there is one, after crediting the time since the last take.
    bool take(Clock::time_point now) {
        if (!started_) {
            started_ = true;
            last_ = now;
        } else if (now > last_) {
            const int64_t earned = (now - last_) / per_token_;
            if (earned > 0) {
                tokens_ = static_cast<uint32_t>(std::min<int64_t>(burst_, static_cast<int64_t>(tokens_) + earned));
                last_ += earned * per_token_;
            }
            if (tokens_ == burst_) last_ = now;   // a full bucket does not save up
        }
        if (tokens_ == 0) return false;
        --tokens_;
        return true;
    }
    uint32_t tokens() const { return tokens_; }

private:
    uint32_t          burst_;
    Clock::duration   per_token_;
    uint32_t          tokens_;
    bool              started_ = false;
    Clock::time_point last_{};
};

/// The value of option `key` in a URL's option string ("?Name=cyb?Team=1"), the key compared without case as UE does
/// (UGameplayStatics::ParseOption); "" when absent.
std::string url_option(std::string_view url, std::string_view key);
// [C O A - Cyb]

/// The login identity of a connection: the ?Name= option of its NMT_Login and the unique id. A Login without a name falls
/// back to the character's name, one without a unique id (or with another encoding) to the name option; each fallback is
/// added to `notes`.
bootstrap::LoginIdentity make_login_identity(const std::optional<LoginMessage>& message, const svc::Character& character,
                                             std::vector<std::string>* notes);

/// The PlayerController properties ServerUpdateAccountCombatSettings is echoed on (pkt#620's nine).
inline constexpr std::array<std::string_view, 9> kCombatEchoPaths = {
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MayHarmAngle",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ProximityRadius",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.MaxDistance",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.ForwardOffset",
    "CharacterInGameSettings.CombatSettings.Targeting.TabTarget.TabTargetMethod",
    "CharacterInGameSettings.CombatSettings.Targeting.bEnableHoverCasting",
    "CharacterInGameSettings.CombatSettings.Targeting.SoftLock.VisualsMask",
    "CharacterInGameSettings.CombatSettings.Input.BufferDuration",
    "CharacterInGameSettings.CombatSettings.Rendering.PartyVFX",
};

class PlayerSession {
public:
    /// Reply steps that may wait for the window at once; more are dropped (a client that sends ServerRequestClientLoad
    /// in a loop).
    static constexpr size_t kMaxPendingReplies = 64;
    /// The budgets (the header's "Budgets"): loads and combat-settings echoes, and storage payloads.
    static constexpr uint32_t                  kLoadBurst = 4;
    static constexpr std::chrono::seconds      kLoadInterval{2};
    static constexpr uint32_t                  kStorageBurst = 16;
    static constexpr std::chrono::milliseconds kStorageInterval{100};
    /// Ability triggers (plan A7): a burst of eight, then ten a second (a Dodge is at most one every 0.65 s).
    static constexpr uint32_t                  kAbilityBurst = 8;
    static constexpr std::chrono::milliseconds kAbilityInterval{100};
    /// plan D2, stage 2: RequestHitVerification_Server and ServerNotifyHeldAbilityCancelled each take a token of a bucket of their own (a Saga sends two
    /// requests at once, a channel of the retail captures 500 in a cast; a client that sends more is dropped and counted); the confirmations waiting to
    /// go out are bounded, and so is what one ClientHitConfirmationsReliable carries (retail: at most 20 hits)
    static constexpr uint32_t                  kHitBurst = 40;
    static constexpr std::chrono::milliseconds kHitInterval{25};
    static constexpr uint32_t                  kCancelBurst = 16;
    static constexpr std::chrono::milliseconds kCancelInterval{50};
    static constexpr size_t                    kMaxPendingConfirms = 256;
    /// The most lingering-zone operations (opens, fades, closes) waiting at once.
    static constexpr size_t                    kMaxZoneOps = 64;
    static constexpr size_t                    kConfirmsPerCall = 20;
    /// Plan D1, stage 2: ServerPurchaseSkillBuild (a click is one request; a swap, a build or a respec one more) and ServerSetActionbarSlotUseData
    /// (a drag is one): a burst of eight and sixteen, then ten a second.
    static constexpr uint32_t                  kTalentBurst = 8;
    static constexpr std::chrono::milliseconds kTalentInterval{100};
    static constexpr uint32_t                  kBarBurst = 16;
    static constexpr std::chrono::milliseconds kBarInterval{100};
    /// The ServerSetActionbarSlotUseData a purchase makes the client send (one per bought active, all in the frame the reply arrives in) have an
    /// allowance of their own, so a purchase of 20 or 30 skills does not use up the bar budget: one credit per skill update sent, at most this
    /// many saved up.
    static constexpr uint32_t                  kBarPurchaseAllowance = 64;
    /// How long after the request a purchase's reply (the pawn's update, the XPTracker, the skill updates) goes out: retail's median, 84 ms
    /// (116 of 123 replies in one packet). The client's bar animation is reported to depend on the reply not arriving in the frame of the
    /// request. 0 sends it at once. The environment variable AOC_PURCHASE_REPLY_DELAY_MS (0-2000) sets it for a session joining after it is set.
    static constexpr std::chrono::milliseconds kPurchaseReplyDelay{84};
    /// The seconds of regeneration one tick may make up for (a session that was not ticked for a while does not get minutes of it at once).
    static constexpr int64_t                   kMaxRegenSteps = 5;
    /// Plan B2: ServerItemUsed and ServerRequestStorageEvent: a burst of eight, then ten a second (a double click is one request); and the
    /// corrections of refused requests (a Backpack and an Equipment payload, 20 KB at most): four at once, then one every two seconds.
    static constexpr uint32_t                  kItemBurst = 8;
    static constexpr std::chrono::milliseconds kItemInterval{100};
    static constexpr uint32_t                  kResyncBurst = 4;
    static constexpr std::chrono::seconds      kResyncInterval{2};
    /// A displacement faster than this between two accepted moves counts as moving even when the move's input says none
    /// (a player cannot sprint for free by sending no input): a slow walk is 450 cm/s, the captures' standing jitter is 0.
    static constexpr double                    kMovingSpeed = 30.0;
    /// Mount interactions, dismounts and dash triggers (the mounts milestone, stage 3): a burst of eight, then ten a second (capture 914 gets on and off 16 times in a minute).
    static constexpr uint32_t                  kRideBurst = 8;
    static constexpr std::chrono::milliseconds kRideInterval{100};
    /// How many of each client-driven line are logged at info before the rest go to debug.
    static constexpr size_t kLoggedPerKind = 3;
    /// The mounts milestone, stage 3 fix 2: the lines of a mount item used, a summon, a mount-up and a dismount are logged at info this many times (the limit of three hid
    /// every use after the third: the log could not say whether a flyer was ever asked for).
    static constexpr size_t kLoggedMounts = 40;
    /// How many of a session's ability transitions (a Sprint or a Dodge starts or ends, a Rest, a refusal's answer) are logged at
    /// info: what an in-game run reads.  A client that Sprints in a loop does not fill the log; the rest are at debug.
    static constexpr size_t kLoggedAbilityTransitions = 100;

    /// Builds the player's setup and bootstrap, gives the four actors their channels and queues the join sequence,
    /// sending what the Connection takes now. nullptr, with `error` set to one sentence for the log, when the setup is
    /// refused (a race id nobody knows, a class content.db lacks ...) or the bootstrap does not build; `refusal`, when
    /// given, says which kind (the setup's own SetupRefusal, Other for everything else).
    ~PlayerSession();

    static std::unique_ptr<PlayerSession> join(Connection& conn, PlayerWorld& world, const svc::Character& character,
                                               const bootstrap::LoginIdentity& login, double world_time,
                                               std::string& error, bootstrap::SetupRefusal* refusal = nullptr);

    /// Plan D1: the EffectId keys of the passives a level grants (an infinite CurrentEffects item each), apart from the ability
    /// runtime's own counter (which starts at 0).
    static constexpr int64_t kFirstPassiveEffectId = 1000000;

    /// Sends what is queued and can go (the join first, then the replies).
    void tick(Connection& conn);

    /// tick(conn), then the periodic save: a changed position is handed to the PositionSink every save_interval (the
    /// interval starts at the first tick after the join). `now` is the Endpoint's clock.
    void tick(Connection& conn, Clock::time_point now);

    /// The session is over (`why` is for the log): hands the last good position to the PositionSink when it differs from
    /// what the record held at login (or a hand-over was made before: see the header). Once; later calls do nothing.
    void finish(const char* why);

    /// A bunch the client sent on an actor channel, at `now` (the Endpoint's clock, for the budgets and the movement
    /// pacing): its RPCs are answered (the table above), except a correction, which waits for end_of_datagram.
    void on_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now);

    /// The datagram's bunches have all been given to on_bunch: sends the one correction its refused moves left, if any
    /// (the latest refusal's, unless a later move of the datagram was accepted).
    void end_of_datagram(Connection& conn);

    /// Takes the Connection's ack events and marks the exports the client acknowledged.
    void take_acks(Connection& conn);

    /// True once a chain of the join could never be sent: the player cannot be spawned.
    bool failed() const { return failed_; }
    const std::string& failure() const { return failure_; }
// [C | O | A :: C-Y-B]

    const bootstrap::PlayerBootstrap& bootstrap() const { return pb_; }
    const std::string& name() const { return pb_.setup().name; }
    /// The actor channel of a role.
    uint32_t channel_of(bootstrap::Role role) const { return channels_[static_cast<size_t>(role)]; }
    /// Sends of the join not on the wire yet, and reply steps waiting.
    size_t join_pending() const { return join_.size(); }
    size_t replies_pending() const { return replies_.size() + delayed_.size(); }
    /// How long a purchase's reply waits after the request (see kPurchaseReplyDelay); a test sets 0 to have it at once.
    void   set_purchase_reply_delay(std::chrono::milliseconds d) { purchase_delay_ = d; }
    std::chrono::milliseconds purchase_reply_delay() const { return purchase_delay_; }
    size_t moves_acknowledged() const { return moves_acked_; }
    /// Plan A6: the checker (the last good position, the limits), the moves refused and corrected, the positions handed
    /// to the PositionSink.
    const MoveChecker& movement() const { return checker_; }
    size_t moves_refused() const { return moves_refused_; }
    /// Moves dropped without a reply: their TimeStamp did not rise (duplicates, reordered datagrams).
    size_t moves_stale() const { return moves_stale_; }
    size_t corrections_sent() const { return corrections_; }
    size_t positions_handed_over() const { return handed_over_; }
    const std::string& character_id() const { return character_id_; }
    /// Plan B2: the items as the session plays them, whether they changed this session and since the last hand-over to the sink, the requests
    /// applied and refused (ServerItemUsed and ServerRequestStorageEvent), and the inventories handed to the sink.
    const items::Inventory& inventory() const { return inventory_; }
    /// Plan B3: the character's stats as the session plays them (null for a session built without them).
    const stats::CharacterStats* character_stats() const { return stats_ ? &*stats_ : nullptr; }
    /// Gear changes that changed stats, the stat items they changed, and the ones the pawn would not take.
    size_t stat_updates() const { return stat_updates_; }
    size_t stat_items_changed() const { return stat_items_changed_; }
    size_t stat_updates_failed() const { return stat_updates_failed_; }
    bool   inventory_changed() const { return inventory_changed_; }
    bool   inventory_dirty() const { return inventory_dirty_; }
    /// The inventory this session started from came from a session that a second login replaced, and it differs from what the character
    /// record held: the old session's final save is all that holds it in the database, and the saver may have lost it. Marks the inventory
    /// as changed this session, so finish() hands it over once more (the latest save wins, so one more is harmless).
    void   inventory_may_be_unsaved() { inventory_changed_ = true; }
    size_t item_requests_applied() const { return items_applied_; }
    size_t item_requests_refused() const { return items_refused_; }
    size_t inventories_handed_over() const { return inventories_handed_over_; }
    /// Plan D1: the character's Adventuring progress and class as the session plays them, the uses applied and refused, the levels gained
    /// and the ranks the Universal tree holds, and the progress hand-overs to the sink.
    const leveling::Progress&   progress() const { return progress_; }
    int                         class_id() const { return class_id_; }
    /// The Universal tree's list as the client was told (the ranks a level grants, then the bought ones).
    std::vector<int64_t>        universal_ranks() const { return pb_.tree_ranks(content::kGuidUniversalTree); }
    size_t uses_applied() const { return uses_applied_; }
    size_t uses_refused() const { return uses_refused_; }
    size_t levels_gained() const { return levels_gained_; }
    /// Gathering stage 2: the character's 22 professions as the session plays them (certification tier, level, XP; not valid() when the world has no profession tables), and
    /// the profession levels gained by items.
    const professions::Professions& professions() const { return professions_; }
    size_t profession_levels_gained() const { return profession_levels_gained_; }

    // ---- Gathering stage 2, part "Harvesting" (session/player_gather.cpp; gather/gathering.h has the rules and the data) ----------------------------------------
    /// The resource node this player is harvesting (its actor id in the registry; 0: none).
    uint64_t harvesting_node() const { return gather_.node; }
    size_t   harvests_started() const { return harvests_started_; }
    size_t   foliage_interacts() const { return foliage_interacts_; }
    // ---- Gathering stage 3, part "Foliage gathering" (session/player_foliage.cpp; foliage/foliage.h has the world's side) ----------------------------------------------------
    /// The instance of the painted foliage (a row of foliage/foliage_map.h) this player is cutting; kNoInstance when none.
    static constexpr uint32_t kNoInstance = 0xFFFFFFFFu;
    uint32_t cutting_instance() const { return foliage_.row; }
    size_t   foliage_started() const { return foliage_started_; }
    size_t   foliage_completed() const { return foliage_completed_; }
    size_t   foliage_cancelled() const { return foliage_cancelled_; }
    size_t   foliage_refused() const { return foliage_refused_; }
    size_t   foliage_interrupts_sent() const { return foliage_interrupts_sent_; }
    const std::map<std::string, size_t>& foliage_refusals() const { return foliage_refusals_; }
    size_t   harvests_completed() const { return harvests_completed_; }
    size_t   harvests_cancelled() const { return harvests_cancelled_; }
    size_t   harvests_refused() const { return harvests_refused_; }
    /// The refusals by kind (the check that failed), and the piles a full bag made.
    const std::map<std::string, size_t>& harvest_refusals() const { return harvest_refusals_; }
    size_t   harvest_piles() const { return harvest_piles_; }
    size_t progress_handed_over() const { return progress_handed_over_; }
    bool   progress_changed() const { return progress_changed_; }
    /// Plan D1, stage 2: the character's ranks (null when the world has no talents catalog), its action bar, the KnownAbilities and the
    /// ReplicatedRecordsToLoad as the client was told, the purchase requests applied and refused, the slot sets applied and refused, the weapon
    /// swaps, and the regeneration ticks that changed Health or Mana.
    const talents::Book*            talents() const { return book_ ? &*book_ : nullptr; }
    const actionbar::Bar&           action_bar() const { return bar_; }
    const std::vector<int64_t>&     known_abilities() const { return known_; }
    const std::vector<int64_t>&     record_abilities() const { return records_; }
    /// The pawn updates that added a carried item's ability to the records after the join (a mount item that reached the bag).
    size_t                          item_ability_syncs() const { return item_ability_syncs_; }
    size_t talent_requests_applied() const { return talents_applied_; }
    size_t talent_requests_refused() const { return talents_refused_; }
    size_t bar_sets_applied() const { return bar_sets_; }
    size_t bar_sets_refused() const { return bar_refused_; }
    size_t weapon_swaps() const { return weapon_swaps_; }
    bool   using_ranged_weapon() const { return use_ranged_; }
    size_t regen_ticks() const { return pipeline_ ? pipeline_->ticks_regen() : 0; }
    /// Marker effect items (CurrentEffects) whose end the server has yet to send.
    size_t effect_markers_pending() const { return markers_.size(); }
    /// Plan A7: the ability runtime (null until the first tick or ability request), the triggers handled, the triggers dropped
    /// for the budget, and the ability updates and ClientEndAbility calls sent.
    const AbilityRuntime* abilities() const { return abilities_ ? &*abilities_ : nullptr; }
    /// The game's own events set a current value (damage, a fall; the tests do): Health, Mana or the class resource becomes `value` on the pawn and in
    /// the session's stats, sent with the next update. False for a stat that is not a current value, has no item, or when abilities are off.
    bool set_current_value(int64_t record, float value);
    /// Plan D2: the generic ability runtime (null before the first tick or when abilities are off).
    const AbilityPipeline* pipeline() const { return pipeline_ ? &*pipeline_ : nullptr; }
    size_t ability_triggers() const { return ability_triggers_; }
    size_t ability_triggers_dropped() const { return ability_dropped_; }
    /// plan D2, stage 2: the hit requests taken, dropped (budget), answered with a confirmation, and the confirmations sent / waiting
    /// Damage that reaches the character (the tests, and what the combat of milestone E will call): the shields take it first, then Health; the
    /// change goes out like any ability event. Returns what got through to Health.
    double take_damage(Connection& conn, double amount, Clock::time_point now);
    /// An NPC's hit on the player, as the combat world delivers it (the session's own tick drains the queue the port fills; the tests hand a hit over directly): the
    /// shields and Health take it, or, while the player rides, its mount's Health does (the mounts milestone, stage 3).
    void take_hit(Connection& conn, const combat::IncomingHit& hit, Clock::time_point now);
    /// The character may trigger `ability` from now on, as if it had bought its rank (a GM's, the tests'): the client's own list is not changed.
    void teach_ability(int64_t ability);
    size_t hit_requests() const { return hit_requests_; }
    size_t hit_requests_dropped() const { return hit_dropped_; }
    size_t hit_confirmations_sent() const { return confirms_sent_; }
    size_t hit_confirmations_pending() const { return pending_confirms_.size(); }
    size_t held_cancels() const { return cancels_; }
    size_t held_cancels_dropped() const { return cancels_dropped_; }
    /// Plan D2, review: the lingering zones' actors the session opened and closed, and how many it has open now.
    size_t zones_opened() const { return zone_opened_; }
    size_t zones_closed() const { return zone_closed_; }
    size_t zone_actors() const { return zone_actors_.size(); }
    size_t zone_ops_pending() const { return zone_ops_.size(); }
    size_t ability_updates_sent() const { return ability_updates_; }
    size_t ability_ends_sent() const { return ability_ends_; }
    /// ClientEndAbility calls waiting for room in the connection.
    size_t ability_ends_pending() const { return pending_ends_.size(); }
    bool   abilities_failed() const { return abilities_failed_; }
    /// Chains whose export parts have no final ack event yet (0 once everything sent is acknowledged or lost).
    size_t exports_tracked() const { return exports_.size(); }
    /// Requests dropped for their budget.
    size_t requests_dropped() const { return dropped_; }
    /// Notes the setup produced (the default spawn used ...), for the log at join.
    const std::vector<std::string>& setup_notes() const { return notes_; }
// (CoA : C:Y:B)

    // ---- plan E1: the living area and the GM vendor ----------------------------------------------------------------------------------
    /// The most an interaction may be from the NPC (cm, horizontal, from the last good position).
    static constexpr double                    kInteractRangeCm = 2500.0;
    /// Vendor requests (an interaction, a purchase): a burst of eight, then ten a second.
    static constexpr uint32_t                  kVendorBurst = 8;
    static constexpr std::chrono::milliseconds kVendorInterval{100};
    /// Tax-table requests (the answer is 7 KB): a burst of three, then one a second.
    static constexpr uint32_t                  kTaxBurst = 3;
    static constexpr std::chrono::milliseconds kTaxInterval{1000};
    // ---- plan E1, stage 3, part 2: kills, loot ---------------------------------------------------------------------------------------
    /// Loot requests (a window opened, items taken): a burst of eight, then ten a second.
    static constexpr uint32_t                  kLootBurst = 8;
    static constexpr std::chrono::milliseconds kLootInterval{100};
    /// The most stacks one request takes (a corpse holds a handful).
    static constexpr size_t                    kMaxLootTakes = 16;
    /// The corpse whose loot window is open (0: none), and the windows opened, the stacks taken, the takes refused.
    uint64_t loot_actor() const { return loot_actor_; }
    size_t   loot_opens() const { return loot_opens_; }
    size_t   loot_taken() const { return loot_taken_; }
    size_t   loot_refused() const { return loot_refused_; }
    /// Plan E1, stage 3, part 2: the player's death.  Dead (Health reached 0 and the player has not chosen where to come back), the deaths and respawns,
    /// the pile the last death left (its actor id, 0: none).  The respawn record the session offers (Player_Respawn_Static: a data record of the
    /// PlayerRespawn type, the choices list needs a record the client knows).
    static constexpr int64_t                   kRespawnRecord = 6064632337807835941ll;
    bool     dead() const { return dead_; }
    size_t   deaths() const { return deaths_; }
    size_t   respawns() const { return respawns_; }
    uint64_t pile_actor() const { return pile_actor_; }
    /// The kills whose experience the session applied.
    size_t   kills_rewarded() const { return kills_rewarded_; }
    /// This session's view of the world's other actors (null for a session without a registry).
    const ActorView* actor_view() const { return view_.get(); }
    /// The mounts milestone, stage 2: this player's mount (null for a session of a world without mount tables), and the requests the session took.
    const MountController* mounts() const { return mounts_.get(); }
    size_t mount_item_uses() const { return mount_item_uses_; }
    size_t mount_summon_requests() const { return mount_summon_requests_; }
    size_t mount_desummon_requests() const { return mount_desummon_requests_; }
    size_t mount_requests_ignored() const { return mount_ignored_; }
    // ---- the mounts milestone, stage 3: riding -------------------------------------------------------------------------------------------
    /// The player is on its mount: its moves, abilities and position are the mount's (the controlled actor).
    bool        mounted() const { return ride_ && ride_->riding(); }
    const Ride* ride() const { return ride_ ? &*ride_ : nullptr; }
    /// Where the player is for the saver and for a second login of its character: the last good position, or, while it rides, where it would be put beside its mount.
    GoodPosition last_position() const;
    /// The mount's actor channel in the actor view (0: none).
    uint32_t     mount_channel() const;
    size_t mount_ups() const { return mount_ups_; }
    size_t dismounts() const { return dismounts_; }
    size_t mount_interactions_refused() const { return mount_refused_; }
    size_t moves_on_the_wrong_channel() const { return moves_wrong_channel_; }
    size_t mount_moves_acknowledged() const { return mount_moves_acked_; }
    size_t dashes_started() const { return dashes_; }
    size_t dashes_refused() const { return dashes_refused_; }
    size_t mount_triggers_refused() const { return mount_triggers_refused_; }
    size_t mount_hits() const { return mount_hits_; }
    size_t mount_deaths() const { return mount_deaths_; }
    /// Stage 4: the dismounts made in the air, the mounts desummoned for their distance from the owner, and that distance (cm; a test lowers it).
    size_t air_dismounts() const { return air_dismounts_; }
    size_t distance_desummons() const { return distance_desummons_; }
    void   set_desummon_distance(double cm) { desummon_distance_ = cm; }
    /// The actor whose vendor window the player has open (0: none), and the vendor def it shows.
    uint64_t vendor_actor() const { return vendor_actor_; }
    int64_t  vendor_def() const { return vendor_def_; }
    size_t   vendor_opens() const { return vendor_opens_; }
    size_t   vendor_purchases() const { return vendor_purchases_; }
    size_t   vendor_refused() const { return vendor_refused_; }
    size_t   interactions_ignored() const { return interact_ignored_; }
    /// The tax-table requests of the vendor windows, and the tables sent in answer.
    size_t   tax_requests() const { return tax_requests_; }
    size_t   tax_tables_sent() const { return tax_tables_sent_; }

    // ---- the GM travel tools (session/gm_travel.h) -----------------------------------------------------------------------------------
    /// What a GM teleport did.
    struct GmMoveResult {
        enum class Status : uint8_t { Moved, Dead, OutsideWorld, Busy } status = Status::Moved;
        GoodPosition from, to;
        bool         dismounted = false;
    };
    /// Moves the player to (x, y, z) cm: the path a respawn uses (the move checker's teleport() and a position correction to the client, so its next moves are not corrected).  A
    /// mounted player is dismounted first; the mount stays and desummons by distance.  Not a dead player (Dead), not a point outside the world box (OutsideWorld), not when the
    /// reply queue is full (Busy).  `by` is who asked, for the log; the log has the from and the to.
    GmMoveResult gm_teleport(Connection& conn, double x, double y, double z, Clock::time_point now, const std::string& by);
    /// The GM's run speed multiplier (a factor of MoveSpeedMult, 1: none): the pawn's stat, the mount's while riding, and the move checker's limits.  The caller keeps it inside 0.5 to 10.
    struct GmSpeedChange {
        double previous = 1.0, now = 1.0;
    };
    GmSpeedChange gm_set_speed(Connection& conn, double multiplier, Clock::time_point now, const std::string& by);
    double        gm_speed() const { return gm_speed_; }
    /// The client's own GM teleport RPCs received, and the ones answered (the switch was on and the target was good).
    size_t        gm_rpcs_received() const { return gm_rpcs_; }
    size_t        gm_rpcs_answered() const { return gm_rpcs_answered_; }
    size_t        gm_teleports() const { return gm_teleports_; }

    // ---- Nodes 1, stage 2: the world knows its nodes ----------------------------------------------------------------------------------
    /// The client asks for the summary and the siege summary of every node at login (49 + 49 in the possession bunch) and again from the node window.  Each request is
    /// queued by node (a node already waiting is not queued twice) and answered in retail's order: every siege summary, then every node summary, each in the order
    /// asked; the answers are sent as the connection has room (kMaxNodeRequests bounds the queue).  The GameState's node maps follow the registry's levels: when it
    /// changes, a delta of the difference goes to this client.
    static constexpr size_t kMaxNodeRequests = 256;
    size_t node_requests() const { return node_requests_; }
    size_t node_summaries_sent() const { return node_summaries_sent_; }
    size_t siege_summaries_sent() const { return siege_summaries_sent_; }
    size_t siege_cooldowns_sent() const { return siege_cooldowns_sent_; }
    size_t node_map_updates() const { return node_map_updates_; }
    size_t node_requests_dropped() const { return node_requests_dropped_; }
    size_t node_gm_rpcs() const { return node_gm_rpcs_; }
    size_t citizenship_requests() const { return citizenship_requests_; }
    /// The registry version the GameState's node maps were last brought to.
    uint64_t node_maps_version() const { return node_maps_version_; }

private:
    PlayerSession(bootstrap::PlayerSetup setup, bootstrap::GameStateMode mode, std::string character_id, const MovementConfig& movement,
                  PositionSink* sink, const content::AbilityRules* rules, items::Inventory inventory, const items::ItemCatalog* catalog,
                  std::optional<stats::CharacterStats> character_stats, const PlayerWorld* world, int class_id, leveling::Progress progress)
        : pb_(std::move(setup), mode),
          character_id_(std::move(character_id)),
          checker_(movement.limits, start_of(pb_.setup())),
          saved_(checker_.good()),
          loaded_(checker_.good()),
          sink_(sink),
          save_interval_(movement.save_interval),
          ability_rules_(rules),
          inventory_(std::move(inventory)),
          catalog_(catalog),
          stats_(std::move(character_stats)),
          world_(world),
          class_id_(class_id),
          progress_(progress),
          base_limits_(movement.limits) {}
// (CoA + C:Y:B)

    /// Where the player starts: its spawn point and yaw.
    static GoodPosition start_of(const bootstrap::PlayerSetup& setup) {
        GoodPosition g;
        g.x = setup.x;
        g.y = setup.y;
        g.z = setup.z;
        g.yaw_degrees = setup.yaw_degrees;
        return g;
    }

    enum class Outcome { Sent, Wait, Refused };

    /// One reply, built when it is about to be sent.
    struct Step {
        const char* what;
        std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build;
        bool must_build = false;   // a step that builds nothing is a warning, not a debug line (the phase-in)
    };

    /// What an AckEvent of a chain needs: the GUIDs of each export bunch.
    struct ChainExports {
        std::vector<std::vector<protocol::emit::FIntrepidNetworkGUID>> by_bunch;
        bool   reliable = true;
        size_t outstanding = 0;   // export parts without a final event yet
    };

    bool allocate_channels(const Connection& conn);
    void pump(Connection& conn);
    Outcome send(Connection& conn, bootstrap::Send& send);
    void queue(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build,
               Connection& conn, bool must_build = false);
    /// Like queue(), but the step goes out purchase_delay_ after `now` (at once when that is 0 and nothing waits): the steps of one request
    /// keep their order, and run_timers() moves each to the replies when its time has come.
    void queue_later(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build, Connection& conn,
                     Clock::time_point now, bool must_build = false);
    /// Whether `steps` more reply steps fit under kMaxPendingReplies (logs once when not).
    bool room_for(size_t steps);
    /// Whether `budget` has a token for a `what` at `now`; counts and (the first time) logs the drop when not.
    bool within_budget(RequestBudget& budget, const char* what, Clock::time_point now);
    /// info for the first `limit` (kLoggedPerKind) of a line (`count` is that line's counter, incremented), debug after.
    static spdlog::level::level_enum level_for(size_t& count, size_t limit = kLoggedPerKind);
    void dispatch(Connection& conn, bootstrap::Role role, const protocol::repl::ClientRpc& rpc, const InBunch& bunch,
                  Clock::time_point now);
    /// What the reader skipped in a bunch that read (calls on subobjects the client names by path, RPC fields the class has no RPC for): counted, and said once per name.
    void note_unserved_blocks(bootstrap::Role role, const protocol::repl::ClientBunch& parsed, const InBunch& bunch);
    void on_move(Connection& conn, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount);
    /// Whether the last good location differs from what was loaded or last handed to the sink (`with_yaw`: or its yaw).
    bool position_changed(bool with_yaw) const;
    /// Hands the last good position to the sink (it is saved on a worker thread); `why` is for the log.
    void hand_over(const char* why, bool periodic);
    void on_combat_settings(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    // ---- plan A7 ----
    /// The runtime, made at the first call (its clock starts at `now`); nullptr once it failed or when there are no rules.
    AbilityRuntime* ability_runtime(Clock::time_point now);
    double ability_seconds(Clock::time_point now) const;
    /// `on_mount`: the trigger came on the mount's AbilityComponent (the controlled actor's while the player rides), not the pawn's.
    void on_ability_trigger(Connection& conn, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount = false);
    void on_ability_cancel(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_hit_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_ability_end(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Advances the runtime to `now` and sends what it produced.
    void run_abilities(Connection& conn, Clock::time_point now);
    /// Applies the runtimes' events to the pawn's state and queues the ClientEndAbility calls: A7's, then the generic runtime's.
    void take_ability_events();
    /// Plan D2: the numbers the formulas give A7's abilities for the ranks the character holds (Sprint's speed, the Stamina regeneration).
    void sync_ability_values();
    /// Abilities stop for this session (the pawn's state and the runtime's bookkeeping disagree, or the connection refused an
    /// update): nothing more is sent, so what the client was told stays as it is; the warning says what that is.
    void abilities_off(const std::string& why);
    /// Whether a Dodge is on, so the move checker allows the dash's speed.
    bool dash_allowed(Clock::time_point now) const;
    /// Sends the pending ability update and then the pending ClientEndAbility calls, as far as the connection has room.
    void flush_abilities(Connection& conn);
    bool is_moving(const GoodPosition& before, const protocol::repl::NetworkMove& move) const;
    void on_storage_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    // ---- plan B2 ----
    void on_item_used(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_storage_event_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Plan B3: recomputes the stats for the Equipment as it is now (and, plan D1, the level, class and rank mods the stats hold) and
    /// applies what changed to the pawn (nothing when nothing did). `what` names the cause for the log.
    void apply_gear_stats(const char* what = "the new gear");
    // ---- plan D1, stage 2 ----
    void on_purchase(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// A refused purchase request: logged, nothing changed, and the lists of the trees it names sent again (budget permitting).
    void refuse_purchase(Connection& conn, const protocol::repl::SkillBuild& build, const std::string& why, Clock::time_point now);
    void on_set_slot(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_set_ranged(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The ranks `granted` become the character's by itself (a level, a class): their abilities join the lists, a passive gets an item (added to
    /// `effects`), a skill with the hotbar flag is put on the bar and announced (`updates`).
    void grant_ranks(const std::vector<talents::RankRef>& granted, Clock::time_point now, bootstrap::AbilityChange& effects,
                     std::vector<protocol::repl::SkillOnActionbar>& updates);
    /// The effect item a rank's passive effect gets (nothing when the rank has none, or for a weapon rank of a line that is not in hand).
    void add_rank_effect(const talents::RankRef& ref, Clock::time_point now, bootstrap::AbilityChange& effects);
    /// Removes the item the session added for a rank's effect, if it still has one.
    void remove_rank_effect(int64_t rank_guid, bootstrap::AbilityChange& effects);
    /// Whether the weapon line in hand or the rank constants changed what the stats are computed from: refreshes the stats' extra mods.
    void refresh_rank_mods();
    /// The class changed live (the record `cls`, ECharacterClass `class_id`): the old class's ranks leave, the new class's tree is listed and its
    /// ranks granted (see the header). `updates` is what the client is told per skill; `cleared` the bar slots to send cleared.
    void switch_class(const content::ClassRecord& cls, Clock::time_point now, bootstrap::AbilityChange& effects,
                      std::vector<protocol::repl::SkillOnActionbar>& updates, std::vector<std::pair<int, int>>& cleared);
    /// Ends the marker effects whose time has come (plan D2's runtime made Health and Mana's regeneration its own).
    void run_timers(Connection& conn, Clock::time_point now);
    /// The bar as one payload (every slot).
    protocol::repl::ActionbarPayload bar_payload() const;
    /// The recomputed KnownAbilities and records for the class tree, ranks and weapon in hand (the join's order).
    void rebuild_ability_lists();
    /// Gives the pawn the session's KnownAbilities and records (what the next pawn update carries).
    bool push_abilities();
    /// The mounts milestone, stage 3 fix 1: the abilities of the mount items the character carries are in the records (the client loads only what they
    /// list, and a mount item used without Summon_Mount loaded fails on the client with "Ability not loaded").  Appends the missing ones, as retail does
    /// when an item reaches the bag (capture 821: one update right after the storage event), and sends the pawn's update.  Called every tick; a no-op
    /// when nothing is missing.
    void sync_item_abilities(Connection& conn);
    /// The seconds since the session's clock started (its first tick or request): what effect items and the ability runtime are timed on.
    double session_seconds(Clock::time_point now);
    // ---- plan D1 ----
    /// A ServerItemUsed on a bag item that is worn in no slot: the item's action responses (item_use.h), applied and answered.
    void on_item_activated(Connection& conn, const protocol::repl::ItemUse& use, const svc::InventoryItem& item, Clock::time_point now);
    /// What a level gained does to the character: the pawn's level, the stats, the ranks and abilities the level grants, the passives' effect items, the
    /// TempHealth mark (item uses and kill experience both end here).  `rank_effects` and `skill_updates` collect what the caller sends.
    void finish_level_up(const leveling::XpResult& xp, bool class_changed, Clock::time_point now, bootstrap::AbilityChange& rank_effects,
                         std::vector<protocol::repl::SkillOnActionbar>& skill_updates);
    /// The AdventuringXPAddMult stat as the character has it (0 when it has none).
    double xp_add_mult() const;
    /// Gathering stage 2: a profession as the XPTracker says it (its leveling type, the XP total, the certification level's guid).
    struct ProfessionWire {
        int64_t leveling_type = 0, xp = 0, certification = 0;
    };
    /// Makes `after` what the character holds in its professions: the stat engine's GetPlayerProfessionLevel answers, the levels gained are counted and the progress is due to be
    /// handed over. Returns the professions that differ (before and after, in profession order); nothing changes for an invalid set or one of another book.
    std::vector<professions::Change> set_professions(const professions::Professions& after);
    /// What the XPTracker is told of these changes (set_profession for each, then the update), in profession order.
    std::vector<ProfessionWire> profession_wire(const std::vector<professions::Change>& changes) const;
    /// BroadcastPlayLevelUpEffects(the profession's leveling type, the level) for each change that gained a level (unreliable, as the Adventuring level-up's).
    void queue_profession_level_ups(Connection& conn, const std::vector<professions::Change>& changes);
    /// Hands the progress (level, class, XP, debt) to the sink; `why` is for the log.
    void hand_over_progress(const char* why);
    /// A fresh instance guid no item of the inventory has (32 lowercase hex digits).
    std::string fresh_guid();
    /// Tells the client what an applied request did (the events, then the visible gear) and marks the inventory dirty.
    void apply_outcome(Connection& conn, const items::Outcome& outcome, const char* what);
    /// A refused request: the reason is logged, the inventory is as it was, and the client's storages are sent again (budget permitting).
    void refuse_item_request(Connection& conn, const char* what, const std::string& why, Clock::time_point now);
    /// Hands the whole inventory to the sink (saved on a worker thread); `why` is for the log.
    void hand_over_inventory(const char* why, bool periodic);
    void on_client_load(Connection& conn, Clock::time_point now);
    // ---- plan E1 ----
    void on_interact(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_vendor_purchase(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_node_tax_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    // ---- Nodes 1, stage 2 ----
    void on_node_summary_request(const protocol::repl::ClientRpc& rpc);
    void on_siege_summary_request(const protocol::repl::ClientRpc& rpc);
    void on_siege_cooldowns_request();
    void on_citizenship_window_request();
    /// ServerSetNodeLevel, ServerAddNodeExperience, ServerGMGetNodeData, ServerGMWhatTimeIsIt, ServerGMFastForwardRealTime: the client's node GM console, answered through the
    /// node service (world/node_control.h) with the GM switch on; every other ServerGM* is logged as not implemented.  Each received is logged, switch or no switch.
    void on_gm_node_rpc(const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// The pending node answers and the registry's changes, sent as the connection has room.
    void run_nodes(Connection& conn);
    void note_node_request(std::deque<int64_t>& wait, std::set<int64_t>& queued, int64_t node, const char* what);
    // ---- the GM travel tools ----
    /// ServerTeleportXYZ (967), ServerTeleport (964) and ServerTeleportPlayer (966) on the PlayerController: logged whatever the switch says; answered only with PlayerWorld::gm_commands.
    void on_gm_teleport_xyz(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_teleport_key(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    void on_gm_teleport_player(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Logs a GM RPC received (always) and whether the switch lets it through; false when it is off or the budget is spent.
    bool gm_rpc_admitted(const char* rpc_name, const std::string& params, Clock::time_point now);
    /// Puts the pawn's MoveSpeedMult stat, the mount's and the checker's limits at the GM multiplier.
    void apply_gm_speed(Connection* conn, Clock::time_point now);
    /// The factor the pawn's horizontal limits are scaled by now: the GM multiplier (never below 1), or, for a second after it went down, the one it was.
    double gm_limit_scale(Clock::time_point now) const;
    void   scale_pawn_limits(Clock::time_point now);
    // ---- the mounts milestone, stage 2 ----
    /// A ServerItemUsed on a mount item (the summon or the desummon follows in the same packet, as its own call): nothing is answered to it, and nothing is consumed.
    void on_mount_item_used(const svc::InventoryItem& item);
    /// A ServerTriggerAbility of Summon_Mount with the item at `slot` of `location`: the summon (or a refusal, or the desummon of the mount that is out).
    void on_summon_trigger(Connection& conn, int32_t client_instance, int32_t slot, uint32_t location, Clock::time_point now);
    /// The PlayerController's ServerDesummonMount.
    void on_desummon_mount(Connection& conn, Clock::time_point now);
    /// Advances the mount to `now` and sends what it made.
    void run_mounts(Connection& conn, Clock::time_point now);
    void apply_mount_effects(Connection& conn, MountController::Effects&& fx);
    /// A hit that does damage reached the player: it cancels a summon in progress.
    void mount_damaged(Clock::time_point now);
    // ---- the mounts milestone, stage 3 ----
    enum class DismountWhy : uint8_t { Request, Desummon, Summon, Ability, PlayerDied, MountDied, MountGone, GmTeleport };
    /// A bunch on the mount's channel (the controlled actor's): routed to the mount's class, its subobjects named by the mount's own.
    void on_mount_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now);
    void dispatch_mount(Connection& conn, const protocol::repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now);
    /// An interaction with the mount's InteractableComponent: getting on.
    void on_mount_interact(Connection& conn, const world::WorldActor& mount, Clock::time_point now);
    bool mount_up(Connection& conn, const world::WorldActor& mount, Clock::time_point now);
    void dismount(Connection& conn, Clock::time_point now, DismountWhy why);
    /// The ground under (x, y) at or below z (a flyer's: a hundred metres down is searched), nullopt when the world has none (no navmesh).  Over a city node's town it is the town's floor
    /// (plus the slack) when the flyer is above it: the baked ground there is a pit about 50 m under the town (world/town_floor.h).
    std::optional<double> ground_below(double x, double y, double z) const;
    /// The PlayerController's ServerDismountInteraction.
    void on_dismount_request(Connection& conn, Clock::time_point now);
    /// Advances the ride (the dash's effects, the cooldown) and sends what it made.
    void run_ride(Connection& conn, Clock::time_point now);
    /// What the rider may do while it rides (the dash, the filter): true when the trigger goes on to the generic runtime.
    bool mounted_trigger(Connection& conn, int64_t ability, int32_t instance, bool on_mount, Clock::time_point now);
    /// A trigger refused: the client's instance is ended on the component it was sent to (the mount's AbilityComponent, or the pawn's).
    void end_instance(Connection& conn, int32_t instance, bool on_mount);
    /// An NPC's hit that reached the mounted player: it lands on the mount.
    void hit_on_mount(Connection& conn, const combat::IncomingHit& hit, Clock::time_point now);
    void mount_died(Connection& conn, Clock::time_point now);
    /// What the mount's item keeps (Health, the lock): written into the bag; the client is told the item changed when the lock is (`conn` set).
    void apply_persist(Connection* conn, const MountController::Persist& p);
    std::optional<bootstrap::Send> mount_ack(float time_stamp);
    std::optional<bootstrap::Send> mount_correction(float time_stamp, const GoodPosition& good);
    // ---- Gathering stage 2, part "Harvesting" ----
    /// A resource node was interacted with: the checks (each logged and counted), then the channel starts (the node is Channeling for everyone).
    void on_gather_interact(Connection& conn, const world::WorldActor& node, Clock::time_point now);
    /// Gathering stage 3, "Hunting" (session/player_gather.cpp): the corpse of a huntable creature was interacted with: only a dead one is hunted; the rest is the node's channel (the
    /// board, the pawn's ChannelingData, the heartbeats), with the creature's Hunting_<kind> interaction (the Bow, its tier) and the corpse writers of gather/node_wire.h.
    void on_hunt_interact(Connection& conn, const world::WorldActor& corpse, Clock::time_point now);
    /// The checks and the start both kinds of channel share: in reach, not busy, the requirements, the length, the board.  `noun` is the log's word ("harvest", "hunt"), `tag` its prefix.
    void begin_channel(Connection& conn, const world::WorldActor& target, const content::GatherDefRow& def, const std::string& what, const char* tag, const char* noun, bool hunt, bool gives_nothing,
                       Clock::time_point now);
    /// The client's ServerInterruptChannel: a harvest in progress ends with no reward.
    void on_interrupt_channel(Connection& conn, Clock::time_point now);
    /// The client's ServerStartInteractionWithFoliage (F pressed on a tree or plant of the world's foliage): the checks (each logged and counted), then the channel starts.
    void on_foliage_interact(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Every tick: respawns, the harvest's conditions (alive, on foot, the same tool, in range), its heartbeat, its completion, and what waits for its time (a pile, a callout).
    void run_gather(Connection& conn, Clock::time_point now);
    /// The channel's time has come: the rolls, the items (the bag or a pile), the tool's wear, the experience, the node gathered.  False when the frame waits for room in the reply queue.
    bool complete_gather(Connection& conn, Clock::time_point now);
    /// Why worldd itself ends a channel, for the client: the callout the retail server shows (the string table "Economy", GatheringInterruption_<key>; only Movement is captured) and,
    /// for a tree or plant, ClientInterruptInteractionWithFoliage [R: 1464]; None: nothing is said (the client ended it, the tool changed, the session ended).
    enum class Interrupt : uint8_t { None, Movement, Death, Mounted, Generic };
    static const char* interrupt_key(Interrupt k);
    /// Ends the harvest in progress (a node's or a tree's or plant's) with no reward; the node is available again.  `conn` may be null (a session that ends).
    void cancel_gather(Connection* conn, const std::string& why, Interrupt interrupt = Interrupt::None);
    /// Ends the cutting of a tree or plant with no reward: the lock released, FoliageData cleared, each logged.  Nothing when none is in progress.
    void cancel_foliage(Connection* conn, const std::string& why, Interrupt interrupt = Interrupt::None);
    /// Gives the client its gathering back: ClientInterruptInteractionWithFoliage, which sets the pawn's FoliageData (the client sets its own at its start) to its cleared value
    /// whatever the parameters are (the instance, its owner and component; none is needed).  A refused start, a cancelled cutting, an unreadable start.
    void unlock_foliage_client(Connection& conn, uint32_t index, const std::string& owner, const std::string& component);
    /// Every tick: the regrowth that is due, the cutting's conditions (alive, on foot, the same tool, still, in reach), the FoliageData 116 ms after the start, the completion.
    void run_foliage(Connection& conn, Clock::time_point now);
    /// The cutting's time has come: the rolls, the items, the tool's wear, the experience, the instance Empty.  False when the frame waits for room in the reply queue.
    bool complete_foliage(Connection& conn, Clock::time_point now);
    /// Tells the client the pawn's UPlayerInfo.FoliageData (the bar and the animation): queued behind what is queued.
    void send_foliage_data(Connection& conn, const bootstrap::PlayerBootstrap::FoliageChannel& data);
    /// The callout of an interruption (Economy / GatheringInterruption_<key>) the movement refusal and every interruption of worldd's own show.
    void send_interrupt_callout(Connection& conn, Interrupt key, Clock::time_point now);
    /// Gathering stage 3: what a finished harvest of any kind (a resource node, a tree or plant of the foliage, a hunting corpse) hands to the common part, `deliver_harvest`.  The
    /// frames' ORDER differs between the kinds (the node clears the pawn's ChannelingData between the items and the XPTracker, the foliage confirms right after the items [R: 1463
    /// packet 833] and clears its FoliageData last), which the two hooks carry; both run at the point of the frame where they are called, `conn` captured by the caller.
    struct HarvestFrame {
        std::string                  what;            // the log's name of the thing harvested ("Plant_Daffodil_Medium_1 (node 12)"); also the pile's record
        int64_t                      profession = 0;  // the profession whose XP the harvest gives
        const gather::Harvest*       harvest = nullptr;
        std::string                  tool_guid;       // the tool's instance and Equipment slot (the wear)
        int                          tool_slot = 0;
        std::function<void()>        after_items;     // queued after the storage events and the full-bag callout, before the XPTracker
        std::function<void()>        after_experience;   // queued after the XPTracker, the level-up frames and the skill updates
        std::string                  log_tail;        // appended to the log line ("; the node returns in 30 s")
        std::string                  tag = "gather";  // the log line's prefix: [gather] for a node, [foliage] for a tree or plant
    };
    /// The common part of a harvest's result: the tool's wear, the rolled stacks into the resource containers (what does not fit goes to a pile 100 ms after the experience frame, with a
    /// callout), the experience and its level-ups, the frames in the retail order, the log line and the hand-overs.  The caller has checked room in the reply queue (`room_for`).
    void deliver_harvest(Connection& conn, Clock::time_point now, const HarvestFrame& frame);
    /// Tells the client the pawn's InteractInfo.ChannelingData (the gather animation and the bar come from it; its end clears it): the property is queued behind what is queued.
    void send_channeling(Connection& conn, const protocol::repl::ChannelingData& data);
    /// The stat run the rolls read: the character's own, at its professions' levels.
    std::unique_ptr<stats::CharacterStats::Evaluation> gather_run(std::string& why);
    // ---- plan E1, stage 3, part 2 ----
    /// An interaction with a corpse's loot component: the window opens (ClientReceiveLootStoragePayload).
    void open_loot(Connection& conn, const combat::Corpse& corpse, const world::WorldActor& actor, Clock::time_point now);
    /// ServerRequestLootStorageEvents: the stacks the client takes go into the bag, the container and the window are told.
    void on_loot_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Health reached 0: the death (the frame of the retail captures, the debt, the loss, the pile).
    void check_death(Connection& conn, Clock::time_point now);
    void die(Connection& conn, Clock::time_point now);
    /// RequestServerRespawnChoices (the client asks 6 s after the death) is answered with SendClientRespawnChoices.
    void on_respawn_choices_request(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// SendServerRespawnChoice(record): the player comes back at the nearest shrine or the login spot, alive, with full Health and Mana.
    void on_respawn_choice(Connection& conn, const protocol::repl::ClientRpc& rpc, Clock::time_point now);
    /// Experience of a kill (the combat world's PlayerPort::receive_kill, queued by the CombatSide): the progress, the XPTracker, a level-up.
    void apply_kill(Connection& conn, const combat::KillReward& kill, Clock::time_point now);
    /// The vendor NPC the window is open on, when it is still held, a GM vendor, and within range of the last good position; else nullptr (the
    /// window is forgotten and `why` says what was wrong).
    const world::WorldActor* open_vendor(std::string& why);
    const protocol::schema::ClassSchema* class_of(bootstrap::Role role) const;
    bool role_of_channel(uint32_t channel, bootstrap::Role* role) const;
    void fail(std::string why);
// [C O A - C Y B]

    bootstrap::PlayerBootstrap pb_;
    std::string character_id_;
    MoveChecker checker_;
    GoodPosition saved_;                 // what was loaded, then what was last handed to the sink
    GoodPosition loaded_;                // what the record held: finish() compares with this, not with a hand-over the saver may have lost
    PositionSink* sink_;
    std::chrono::seconds save_interval_;
    const content::AbilityRules* ability_rules_;   // plan A7: PlayerWorld's, null in a test that wants no abilities
    items::Inventory inventory_;                   // plan B2
    const items::ItemCatalog* catalog_;
    /// The catalog the grants route by (Gathering stage 3); a session without one knows no record, so everything goes to the Backpack.
    const items::ItemCatalog& item_catalog() const;
    bool inventory_dirty_ = false;                 // changed since the last hand-over to the sink
    bool inventory_changed_ = false;               // changed at all this session
    std::optional<stats::CharacterStats> stats_;   // plan B3
    size_t stat_updates_ = 0, stat_items_changed_ = 0, stat_updates_failed_ = 0, stat_logged_ = 0;
    // plan D1
    const PlayerWorld*   world_ = nullptr;           // the content (item uses, the level curve, the auto grants); null in a test without one
    int                  class_id_ = 0;              // ECharacterClass, as the session plays it (a class scroll changes it)
    leveling::Progress   progress_;
    bool                 progress_changed_ = false;  // at all this session
    professions::Professions professions_;           // Gathering stage 2: the 22 professions (not valid() without the world's book)
    size_t               profession_levels_gained_ = 0;
    // Gathering stage 2, part "Harvesting"
    struct GatherRun {
        uint64_t                     node = 0;          // the registry actor being harvested (0: none)
        protocol::emit::FIntrepidNetworkGUID target;    // the node's guid as the client has it (the pawn's ChannelingData names it, and the clear names it again)
        const content::GatherDefRow* def = nullptr;
        int                          tool_slot = 0;     // the Equipment slot of the tool, its instance and the durability it had when the channel began
        std::string                  tool_guid;
        Clock::time_point            started{};
        double                       seconds = 0;
        double                       start_x = 0, start_y = 0;   // where the player stood when the channel began (the stand-still check measures from here)
        bool                         hunt = false;      // Gathering stage 3: the node is a huntable creature's corpse (its channel is a hunt)
        bool                         gives_nothing = false;   // ... of a kind whose reward table gives no usable item (the session drops the items)
        std::string                  what;              // the log's name of what is harvested
    } gather_;
    struct GatherPile {          // the stacks a full bag could not take: an APlayerLootPile 100 ms after the experience frame [R: 1461 packets 897 and 904]
        Clock::time_point due{};
        std::vector<combat::LootStack> stacks;
        double x = 0, y = 0, z = 0;
        std::string node_name;
    };
    std::deque<GatherPile>           gather_piles_;
    struct GatherCallout {       // the "not enough inventory space" callout and when it is taken off
        Clock::time_point due{};
        uint64_t          key = 0;
    };
    std::deque<GatherCallout>        gather_callouts_;
    uint32_t                         gather_callout_seq_ = 0;
    std::map<std::string, size_t>    harvest_refusals_;
    size_t harvests_started_ = 0, harvests_completed_ = 0, harvests_cancelled_ = 0, harvests_refused_ = 0, harvest_piles_ = 0;
    size_t gather_logged_ = 0, gather_refused_logged_ = 0, gather_done_logged_ = 0;
    size_t foliage_interacts_ = 0;   // ServerStartInteractionWithFoliage calls
    bool   gather_off_logged_ = false;
    // Gathering stage 3, part "Foliage gathering": the tree or plant this player is cutting
    struct FoliageRun {
        uint32_t                       row = 0xFFFFFFFFu;   // the instance (foliage_map.h); kNoInstance: none
        int                            state = 0;           // the state it is cut in (the client's and the world's agree)
        uint32_t                       index = 0;           // as the client named it
        std::string                    owner, component;
        const content::GatherDefRow*   interaction = nullptr;
        std::string                    name;                // the def's, for the log
        int                            tool_slot = 0;
        std::string                    tool_guid;
        int64_t                        tool_record = 0;
        Clock::time_point              started{}, data_at{}, completes{};
        double                         seconds = 0;
        double                         start_x = 0, start_y = 0;
        bool                           data_sent = false;
    } foliage_;
    Clock::time_point                moved_at_{};              // the last accepted move that was a walk (a held direction or a speed)
    Clock::time_point                last_now_{};              // the time of the last tick (what a cancel with no time of its own is stamped with)
    size_t                           foliage_started_ = 0, foliage_completed_ = 0, foliage_cancelled_ = 0, foliage_refused_ = 0, foliage_interrupts_sent_ = 0;
    size_t                           foliage_logged_ = 0, foliage_refused_logged_ = 0, foliage_done_logged_ = 0;
    uint32_t                         foliage_callout_seq_ = 0;
    std::map<std::string, size_t>    foliage_refusals_;
    bool                             foliage_off_logged_ = false;
    // plan D1, stage 2
    std::optional<talents::Book> book_;
    actionbar::Bar               bar_;
    std::vector<int64_t>         known_, records_;          // as the client was told
    int64_t                      combo_ = 0;                // the combo of the weapon in hand, as KnownAbilities holds it (0: none)
    bool                         use_ranged_ = false;
    int64_t                      wielded_line_ = 0;         // the weapon skill line of the weapon in hand (0: none)
    std::map<int64_t, int64_t>   rank_effects_;             // rank guid -> the CurrentEffects key of the item the session added for it
    struct Marker {
        Clock::time_point due;
        int64_t           key = 0;
    };
    std::vector<Marker>          markers_;                  // effect items that end by themselves
    RequestBudget                talent_budget_{kTalentBurst, kTalentInterval};
    RequestBudget                bar_budget_{kBarBurst, kBarInterval};
    size_t talents_applied_ = 0, talents_refused_ = 0, bar_sets_ = 0, bar_refused_ = 0, weapon_swaps_ = 0;
    size_t talent_logged_ = 0, talent_refused_logged_ = 0, bar_logged_ = 0, bar_refused_logged_ = 0, regen_logged_ = 0;
    bool   talents_unreadable_logged_ = false;
    Clock::time_point            origin_{};                 // the session's clock 0 (the first tick or request)
    bool                         origin_set_ = false;
    size_t uses_applied_ = 0, uses_refused_ = 0, levels_gained_ = 0, progress_handed_over_ = 0;
    size_t use_logged_ = 0, use_refused_logged_ = 0;
    int64_t next_passive_effect_id_ = kFirstPassiveEffectId;
    RequestBudget item_budget_{kItemBurst, kItemInterval};
    RequestBudget resync_budget_{kResyncBurst, kResyncInterval};
    size_t items_applied_ = 0, items_refused_ = 0, inventories_handed_over_ = 0;
    size_t item_applied_logged_ = 0, item_refused_logged_ = 0, item_periodic_logged_ = 0;
    bool item_unreadable_logged_ = false, activate_logged_ = false;
    std::optional<AbilityRuntime> abilities_;
    std::optional<AbilityPipeline> pipeline_;            // plan D2: every other ability, and Health and Mana's regeneration
    Clock::time_point             abilities_origin_{};   // the runtime's clock 0
    bool                          abilities_failed_ = false;
    bool                          abilities_dirty_ = false;   // changes applied to the pawn and not sent yet
    std::deque<int32_t>           pending_ends_;               // ClientEndAbility calls not sent yet, in order
    RequestBudget                 ability_budget_{kAbilityBurst, kAbilityInterval};
    RequestBudget                 hit_budget_{kHitBurst, kHitInterval};
    RequestBudget                 cancel_budget_{kCancelBurst, kCancelInterval};
    std::deque<bootstrap::HitConfirm> pending_confirms_;       // plan D2, stage 2: confirmations not sent yet, in order
    // plan D2, review: the actors of the lingering zones (a dance's visual effect), each on a channel of its own
    // (plan E1: the actors are the world registry's; the session keeps which zone is which actor)
    struct ZoneActor {
        uint64_t                   actor = 0;      // the actor's ObjectId, in the registry
    };
    std::map<int64_t, ZoneActor>   zone_actors_;               // by the runtime's zone instance id
    std::deque<bootstrap::ZoneOp>  zone_ops_;                  // opens, fades and closes not done yet, in order
    std::optional<protocol::emit::FIntrepidNetworkGUID> pawn_mesh_;   // the pawn's CharacterMesh0 the zones' actors attach to: one guid for the session
    uint64_t                       zone_random_ = 0;
    size_t                         zone_opened_ = 0, zone_closed_ = 0, zone_logged_ = 0;
    void     flush_zones(Connection& conn);
    size_t hit_requests_ = 0, hit_dropped_ = 0, confirms_sent_ = 0, cancels_ = 0, cancels_dropped_ = 0, hit_unreadable_logged_ = 0, hit_unserved_logged_ = 0;
    size_t ability_triggers_ = 0, ability_dropped_ = 0, ability_updates_ = 0, ability_ends_ = 0;
    size_t ability_logged_ = 0, ability_unreadable_logged_ = 0, ability_transitions_logged_ = 0, ability_build_logged_ = 0, item_ability_logged_ = 0;
    // plan E1: the world's other actors, the phase-in and the GM vendor window
    world::ActorRegistry*          registry_ = nullptr;
    PlayerWorld*                   npc_world_ = nullptr;   // plan E1, stage 2: the NPCs' world (the player's place is told to it every tick)
    // plan E1, stage 3: this player in the fights of the area
    class CombatSide;
    std::shared_ptr<CombatSide>               combat_side_;
    std::unique_ptr<combat::WorldHitPort>     hit_port_;
    bool                                      combat_flag_ = false;   // bInCombat as the client was last told
    void drain_combat(Connection& conn, Clock::time_point now);
    void leave_combat();
    /// The actors of the world a call's objects name (an NPC's collision cylinder, by GUID or named in place: the outer is the actor), the caster's
    /// own components left out.
    std::vector<uint64_t> actors_of(const std::vector<protocol::emit::FIntrepidNetworkGUID>& objects,
                                    const std::vector<protocol::repl::InlineObject>& inline_objects = {}) const;
    /// True when an object named in place is one of the caster's own components (its pawn as the outer, or its cylinder).
    bool own_inline_object(const protocol::repl::InlineObject& o) const;
    // plan E1, stage 3, part 2: the loot window and the kills
    RequestBudget                             loot_budget_{kLootBurst, kLootInterval};
    uint64_t                                  loot_actor_ = 0;
    bool                                      dead_ = false;
    size_t                                    deaths_ = 0, respawns_ = 0, death_logged_ = 0;
    uint64_t                                  pile_actor_ = 0;
    double                                    death_x_ = 0, death_y_ = 0, death_z_ = 0;
    size_t loot_opens_ = 0, loot_taken_ = 0, loot_refused_ = 0, loot_logged_ = 0, kills_rewarded_ = 0;
    std::unique_ptr<ActorView>     view_;
    // the mounts milestone, stage 3: the controlled actor
    MoveLimits                     base_limits_;                 // the pawn's: the limits every possession's checker starts from
    // the GM travel tools: the run speed multiplier (a factor of MoveSpeedMult; lasts until the session ends), the one it was a moment ago (the grace after it goes down)
    double                         gm_speed_ = 1.0;
    double                         gm_grace_speed_ = 1.0;
    Clock::time_point              gm_grace_until_{};
    RequestBudget                  gm_budget_{4, std::chrono::milliseconds(250)};
    size_t gm_rpcs_ = 0, gm_rpcs_answered_ = 0, gm_teleports_ = 0, gm_logged_ = 0;
    // Nodes 1, stage 2
    std::deque<int64_t>            siege_wait_, node_summary_wait_;      // the nodes asked for, in the order asked
    std::set<int64_t>              siege_queued_, node_summary_queued_;  // ... and which are waiting (a repeated request is one answer)
    bool                           cooldowns_pending_ = false;
    uint64_t                       node_maps_version_ = 0;               // the registry version the GameState's maps were last brought to
    size_t node_requests_ = 0, node_summaries_sent_ = 0, siege_summaries_sent_ = 0, siege_cooldowns_sent_ = 0, node_map_updates_ = 0, node_requests_dropped_ = 0,
           node_gm_rpcs_ = 0, citizenship_requests_ = 0, node_logged_ = 0;
    bool                           node_unreadable_logged_ = false, node_overflow_logged_ = false, citizenship_logged_ = false, node_unknown_logged_ = false;
    std::optional<Ride>            ride_;                        // the mount the player rides, or has been on and has not lost (desummoned)
    double                         dash_ready_at_ = -1.0;        // review A I3: when the mount dash is ready again (the session's ability clock): a new mount's Ride starts with it, so a desummon and a summon do not reset the 30 s
    protocol::emit::FIntrepidNetworkGUID mount_guid_, mount_cylinder_, mount_ability_, mount_mesh_;   // of the ride's mount
    std::optional<protocol::emit::FIntrepidNetworkGUID> interact_info_;   // the pawn's InteractInfo (MountPtr), minted at the first ride
    RequestBudget                  ride_budget_{kRideBurst, kRideInterval};
    size_t air_dismounts_ = 0, distance_desummons_ = 0;
    double desummon_distance_ = Ride::kDesummonDistanceCm;
    size_t mount_ups_ = 0, dismounts_ = 0, mount_refused_ = 0, moves_wrong_channel_ = 0, mount_moves_acked_ = 0, dashes_ = 0, dashes_refused_ = 0,
           mount_triggers_refused_ = 0, mount_hits_ = 0, mount_deaths_ = 0, ride_logged_ = 0, ride_wrong_logged_ = 0;
    Clock::time_point              dismounted_at_{};             // the moment of the last dismount: the second request of a pair in one datagram sends the extra restart
    bool                           dismounted_valid_ = false, dismount_extra_sent_ = false;
    // the mounts milestone, stage 2
    std::unique_ptr<MountController> mounts_;
    size_t                         mount_item_uses_ = 0, mount_summon_requests_ = 0, mount_desummon_requests_ = 0, mount_ignored_ = 0, mount_logged_ = 0;
    bool                           phase_in_done_ = false;       // the phase-in is built: the view may start
    size_t                         item_ability_syncs_ = 0;      // the pawn updates sync_item_abilities sent
    // E1 stage 3 fix: when the client's synchronized clock is set again (ClientSetTime): 1 s and 5 s after the phase-in, then every 120 s
    static constexpr std::chrono::seconds kTimeSyncAfterPhaseIn{1};
    static constexpr std::chrono::seconds kTimeSyncSecondAfterPhaseIn{5};
    static constexpr std::chrono::seconds kTimeSyncInterval{120};
    std::deque<Clock::time_point>  time_syncs_;
    size_t                         time_syncs_sent_ = 0;
    uint64_t                       vendor_actor_ = 0;
    int64_t                        vendor_def_ = 0;
    RequestBudget                  vendor_budget_{kVendorBurst, kVendorInterval};
    RequestBudget                  tax_budget_{kTaxBurst, kTaxInterval};
    size_t tax_requests_ = 0, tax_tables_sent_ = 0, tax_logged_ = 0;
    size_t vendor_opens_ = 0, vendor_purchases_ = 0, vendor_refused_ = 0, interact_ignored_ = 0, vendor_logged_ = 0, interact_logged_ = 0;
    bool   vendor_unreadable_logged_ = false, vendor_sale_logged_ = false;
    Clock::time_point last_save_{};
    bool save_clock_started_ = false;
    bool finished_ = false;
    std::array<uint32_t, 4> channels_{};
    std::deque<bootstrap::Send> join_;
    std::deque<Step> replies_;
    struct Delayed {
        Clock::time_point due;
        Step              step;
    };
    std::deque<Delayed> delayed_;                       // replies waiting for their time, in the order they were asked for
    std::chrono::milliseconds purchase_delay_ = kPurchaseReplyDelay;
    uint32_t bar_allow_ = 0;                            // ServerSetActionbarSlotUseData the purchases made the client send, not yet seen
    std::unordered_map<ChainId, ChainExports> exports_;
    std::vector<std::string> notes_;
    std::set<std::string> unhandled_logged_;
    bool failed_ = false;
    std::string failure_;
    bool phase_in_requested_ = false;
    bool replies_full_logged_ = false;
    bool waiting_logged_ = false;
    bool blocked_ = false;              // the head of the queue waits for room on blocked_channel_ (it held blocked_unacked_ bunches)
    uint32_t blocked_channel_ = 0;
    size_t blocked_unacked_ = 0;
    size_t malformed_bunches_ = 0;
    /// The kinds of bunch that did not read (their size and first eight bytes), the first of each said whole: a new kind is not hidden by the budget the earlier ones used up.
    static constexpr size_t kMaxUnreadKinds = 16;
    std::set<std::string> unread_kinds_;
    /// Every call that reached dispatch, by the schema's name pointer (the summary at the session's end says them by name).
    std::unordered_map<const char*, size_t> client_calls_;
    /// The client's calls on subobjects it names by path (the reader skips them): blocks, the bunches that held them, each name's count (by "outer ObjectId/path"; the first
    /// kMaxInPlaceNames are said once each), the RPC fields skipped for want of an RPC of the class and how often that was said.
    static constexpr size_t kMaxInPlaceNames = 12, kMaxInPlaceSharedLogged = 20;
    size_t in_place_blocks_ = 0, in_place_bunches_ = 0, in_place_shared_logged_ = 0, skipped_fields_ = 0, skipped_fields_logged_ = 0;
    std::unordered_map<std::string, size_t> in_place_names_;
    size_t foliage_unreadable_logged_ = 0;   // the foliage starts whose parameters did not decode and were said (the first few)
    size_t moves_acked_ = 0;
    size_t moves_refused_ = 0, moves_stale_ = 0, corrections_ = 0, handed_over_ = 0, moves_borderline_ = 0;
    /// Moves dropped as TimeStampNotRising in a row (reset by any other verdict): a streak is logged once at info (the death of 2026-10-10 dropped 66 s of moves and showed nothing at info).
    size_t stale_streak_ = 0;
    bool   stale_streak_logged_ = false;
    static constexpr size_t kStaleStreakLog = 30;
    /// Where the player is, every 30 s at info (the periodic save's line is logged for the first three saves only).
    Clock::time_point last_position_log_{};
    bool position_log_started_ = false;
    static constexpr std::chrono::seconds kPositionLogInterval{30};
    void log_position(Clock::time_point now);
    /// The ground speed diagnostic (logged when a Sprint ends): distance (cm) and TimeStamp seconds of accepted moving steps,
    /// sprinting and not, Dodges left out.
    struct SpeedTally { double cm = 0, seconds = 0; };
    SpeedTally sprint_speed_, run_speed_;
    bool sprint_seen_ = false;
    size_t speed_logged_ = 0;
    void tally_speed(const GoodPosition& before, const GoodPosition& after, bool sprinting, bool evading);
    std::optional<float> pending_fix_;   // the TimeStamp of the latest refused move of this datagram: its correction waits for end_of_datagram
    bool unreadable_move_logged_ = false;
    size_t refused_logged_ = 0, periodic_save_logged_ = 0;
    // the budgets and what they dropped
    RequestBudget load_budget_{kLoadBurst, kLoadInterval};
    RequestBudget combat_budget_{kLoadBurst, kLoadInterval};
    RequestBudget storage_budget_{kStorageBurst, kStorageInterval};
    size_t dropped_ = 0;
    // how often each client-driven line was logged (info up to kLoggedPerKind, debug after)
    size_t possession_logged_ = 0;
    size_t load_logged_ = 0;
    size_t combat_logged_ = 0;
    size_t combat_empty_logged_ = 0;
    size_t reply_sent_logged_ = 0;
};

/// The ItemEvents the bootstrap sends for an Outcome's events (Gathering stage 3: the one conversion, for the session and for a harvest).
std::vector<bootstrap::ItemEvent> item_events_for(const items::Outcome& outcome);

/// A joined player and its connection, as the Endpoint lists them (Endpoint::players) for the GM travel tools; both null for none.
struct PlayerHandle {
    PlayerSession* player = nullptr;
    Connection*    connection = nullptr;
    explicit operator bool() const { return player != nullptr && connection != nullptr; }
};

}}  // namespace aoc::worldd
