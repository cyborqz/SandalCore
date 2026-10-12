# libaocnet

C++20 static library with the AoC / IntrepidNet protocol code: everything
needed to read and write the bits on the wire, independent of any server.

| Directory | Contents |
|---|---|
| `protocol/conn/` | The connection (plan A4): 14-bit sequence arithmetic (`sequence.h`), packet acks ported from UE5 FNetPacketNotify (`packet_notify.h`), the stateless handshake with Intrepid's 94-byte extension and the cookie-derived initial sequences (`handshake.h`), data-packet framing in both directions and the control-channel bunch header (`packet_framing.h`), NMT messages (`nmt.h`). Tested against the retail client's captured packets (`tests/capture_packets.h`) |
| `protocol/wire/` | Bit primitives (`ue5_primitives.h`), packet header and PacketInfo (`packet_info.h`), bunch headers and `walk_bunches` (`packet_parser.h`), partial-bunch reassembly, RPC payload codecs (`actor_rpc_parser.h`, `level_visibility.h`, `hit_confirmation.h` (superseded by `repl/hit_codec.h`), `server_move_parser.h`) |
| `protocol/emit/` | Bunch writer, `ActorBuilder`, NMT builder, package-map exports, property-update bunches, RepLayout codecs (`replayout/`) |
| `protocol/schema/` | The older hand-derived class schemas and the schema registry (`actor_rep_handles.h`, the per-actor `*_schema.cpp`); new code uses `generated/` |
| `protocol/schema/generated/` | The client's net classes as C++ tables (RepLayout commands, field tables, RPC parameters), generated from the client exe by `tools/ghidra/schemagen.py`; `MANIFEST.sha256` and `schemagen.py --check` keep them honest |
| `protocol/repl/` | The replication core (plan A4R): replicated values, per-object state, NetSerialize codecs, RepLayout blocks with a per-connection shadow, fast arrays, RPCs, exports and content blocks, and the replicator that emits an actor's open, updates and RPCs for worldd's `Connection`. Tested bit for bit against the original server's captured bunches |
| `protocol/net_guid_allocator.h` | NetGUID allocation per player (the legacy server) |
| `protocol/server_guid_allocator.h` | NetGUIDs minted under a world server's ServerId (worldd) |

## Boundary rule

Files here may include only the C++ standard library, `spdlog`, and headers
under `libs/aocnet`. The `aocnet` target's only include directory is this
folder, so anything else fails to compile. Replay-era code (`src/protocol/
bootstrap`, `actors`, `emit/replay_mutator`) stays in `src/`.

Include paths keep the `protocol/` prefix: `#include "protocol/wire/packet_parser.h"`.

Two gaps in that enforcement:

- A header is only checked when some translation unit of the `aocnet` target
  or its tests includes it. `net_guid_allocator.h` and `wire/server_move_parser.h`
  are compiled only by legacy targets (`aoc_server`, some `src/tools` tests)
  that have `src/` on their include path, so a forbidden include there would
  first fail in a new consumer such as worldd.
- Never add a file under `src/protocol/{wire,emit,schema}/` with the same name
  as one here. `aoc_server` searches `src/` first, so its own files would pick
  up the `src/` copy while the `aocnet` objects use this one.

Consumers on Windows get `NOMINMAX` from the target (`ue5_primitives.h` calls
`std::min`).

## Class schemas (`protocol/schema/generated/`)

The client's net classes as C++ tables, generated from the exe so nothing in them is guessed.
`tools/ghidra/schemagen.py` reads `AOCClient-Win64-Shipping.exe` (read-only; the path is `EXE`
in `tools/ghidra/ue_net_functions.py`) and writes one header per class and per super class:
49 schemas, 45 native and 4 Blueprint (`PlayerPawn_C`, `AoCPlayerControllerBP_C`,
`AoCGameStateBP_C`, `AoCPlayerStateBP_C`); the mounts milestone (stage 1) added `UMountComponent` (12 commands, 11 net fields, two RPCs: `ItemInstanceGuid` 5-8, `bIsDesummoning` 9, `bIsSummoning` 10, `DissolveTimer` 11; `find_component_schema("GroundMountComponent" | "FlyingMountComponent" | "AquaticMountComponent")` and `find_class_schema("MountComponent_C")`, an alias in `CLASS_ALIASES` with no header of its own: the Blueprint component class the retail opens export it as, which adds no replicated variable); plan D2 (review) added `AAoCAbilityLingeringEffect`, the actor a Bard dance's visual effect is on the client
(32 commands, 21 fields, no RPC; its retail opens are in `server/worldd/tests/capture_lingering.h`); plan D1 added `ULevelingComponent`, the PlayerController's `XPTracker`
(five fast maps and three RPCs; `find_component_schema("XPTracker")`); plan E1, stage 3, part 2 added `ULootComponent` (`OwnerGuid`, `CosmeticData.TopRarity`, `ServerLootContainer`) and `APlayerLootPile` (the actor a dead player's materials fall into; its three loot components are dynamic subobjects, as a corpse's); plan E1 added the other actors' classes: `ANPCCharacter` (70 commands, 91 net fields: ReplicatedMovement, Owner, Controller, the timestamp,
`CurrentMovementInput`), `AGatherableActor` (28 commands: `GatherableGuid` 23-26, `bGathered` 27), and the components of an NPC and of a node: `UNPCInfo` (the subobject every NPC calls `BaseCharacterInfo`:
`CharacterGuid`, `Level`, `CreatureOwnerName`), `UNPCCombatInfo`, `UNPCSecondaryInfo` (`InteractInfo`: `PopInstanceId` 34, `PopAssetId` 35) and its parent `UCharacterSecondaryInfo`, `UNPCAppearanceComponent`,
`UInteractableComponent` (`InteractVisibleStateMap`; four RPCs, none of them the client's interaction request) and `UAoCNodePlotComponent`; `find_component_schema("InteractInfo")`, `("NPCAppearanceComponent")`, `("InteractableComponent")` and
`("AoCNodePlot")` name them. Each holds three tables, flattened super class first
(`schema_types.h`, the one hand-written file, describes them):

- RepLayout commands: the wire handle (1-based), the dotted path (`AttachmentReplication.AttachParent`),
  the property type, the wire width and, for a NetSerialize struct, its name. `commands[handle - 1]`.
- Net fields: the property and RPC field indices of the class, NumFields (the `SerializeInt` maximum
  of a field header) and the function flags.
- RPC parameters: per RPC, its parameters flattened into commands the way `FRepLayout::InitFromFunction`
  does.

`index.h` finds a schema by native or Blueprint class name (`find_class_schema("AoCPlayerControllerBP_C")`),
a pawn component by its export name (`find_component_schema("BaseCharacterInfo")`), or lists all of them.
The rules the generator follows (`UClass::ClassReps`, `FClassNetCacheMgr`, enum widths, ...) are in
`schemagen.py`'s docstring with their UE 5.6 source functions; every exe address is in the header
that uses it.

```
python tools/ghidra/schemagen.py            # rewrite what changed, and MANIFEST.sha256 (about 50 s)
python tools/ghidra/schemagen.py --check    # regenerate in memory and compare; exit 1 on any difference
```

`MANIFEST.sha256` lists the generated files (every header and `index.cpp` but `schema_types.h`) as
`sha256sum` lines, hashed with LF line endings so a CRLF checkout matches. `--check` needs only the exe:
it fails when the manifest, a file on disk, or the regenerated text differ, or when the directory holds a
header the tool does not write. Run it after touching `schemagen.py`, `ue_class_props.py`,
`ue_replayout.py` or `ue_net_functions.py`, and whenever the exe changes.

The Blueprint classes, found in the cooked packages (`tools/ghidra/bp_class_props.py`):

- They add no replicated variable: the class member lists carry no `CPF_Net` property (`PlayerPawn_C` 56
  members, `AoCPlayerControllerBP_C` 37, `AoCPlayerStateBP_C` 1, `AoCGameStateBP_C` 3), and the captured
  PlayerController open decodes to its last bit on native handles alone. A Blueprint class's RepLayout
  commands are its native parent's.
- `PlayerPawn_C` (4) and `AoCPlayerControllerBP_C` (6) do add replicated Blueprint events (`FUNC_Net`
  functions). They extend the field table after the parent's fields, sorted by name
  (`BLUEPRINT_NET_FUNCTIONS` in `schemagen.py`).
- `schemagen.py --bp-assets <Content dir>` re-checks both facts against the cooked packages.

Two property types are not modelled, `TOptional` and `TSet`, in four properties: `AAoCGameStateBase`'s
`Quests.GeneralQuestMarker`, and `AAoCPlayerController`'s `QuestInstance.QuestObjectiveMarker.MapMarkerOverride`,
`SpawnQueryResults.ChildSocketGuids` and `.UsedChildSocketGuids`. schemagen warns, the header carries a NOTE,
and the command has `PropType::Unknown`, which no `Value` fits, so `RepState::set` refuses it.
`tools/ghidra/schema_check.py` decodes the captured blocks with these schemas and `fast_array_check.py` the
fast arrays (below); both read `replay_bunches` output:

```
dist\Release\replay_bunches.exe fixtures\replay_data.bin --to 100000 > bunches.jsonl
python tools/ghidra/schema_check.py bunches.jsonl
python tools/ghidra/fast_array_check.py bunches.jsonl
```

## Replication core (`protocol/repl/`)

What an actor is as the server holds it, and what goes out for it, in the form the original server
sent (`fixtures/replay_data.bin`). Every form here was read off the capture and checked bit for bit by
the `test_repl_*` tests; the facts' evidence (packet numbers, exe addresses, UE source functions) is in
each header's top comment.

| File | Contents |
|---|---|
| `value.h` | `Value`: one replicated value, tagged by what the wire carries (bool, integers, floats, FString, FName, FText, object reference, soft object, a NetSerialize struct in its typed form, a dynamic array of element states). Quantized values are held as the integers on the wire, so bits round trip exactly; floats compare by bit pattern |
| `rep_state.h` | `RepState`: an object's values by RepLayout handle. `set(schema, "path", value)` checks the path, the kind and the width, and refuses what the writer would refuse (a NUL in a string, a quantized component over 63 bits, an id over 255 bytes, a struct without a codec), so a bad value fails where it is built and cannot stall the object later |
| `codecs.h` | Readers and writers for FString, FName, FText, objects, soft objects, FVector / FVector2D / FRotator, the `FVector_NetQuantize*` family (`QuantVec`), `FRepMovement`, `FUniqueNetIdRepl`, gameplay tags, `FStatInt32InstRep`, `FCharacterLoadTracker`, and AoC's own structs: FQuat, FGuidToBoolWrapper, FAlignmentContainer, FAppearanceId, FCharacterCustomizationSaveData, `FPlayerMovementInput` (plan E1: four bits, the direction an NPC or a player holds; the last four bits of every one of the 323 NPC opens of capture 957), and (plans A7 and B2) `FItemInstance` and `FItemStorageEvent`, whose codecs are in `item_codec.h`. `CodecOptions` carries what the bytes do not say (FRepMovement's rotation width and quantization levels, which stat Records carry Base and EquipmentValue, whether the client knows a customization preset, and `inline_objects`: whether an object parameter of a client RPC may name its object in place, as a `Value` `InlineObject`, the form the client uses for the pawn's CollisionCylinder in a trigger's Targets). A struct without a codec is refused by name; no reader guesses |
| `rep_block.h` | The RepLayout block (`write_rep_block` / `read_rep_block`): bDoChecksum, handles ascending, handle 0; dynamic arrays as a count and element handles. `RepShadow` is what one connection last got: the first send is every set handle, later ones only the changed handles. `ParentMask` leaves out the properties a connection should not get. The field-loop header (`SerializeInt(field, NumFields + 1)`, packed bit count) is here too |
| `fast_array.h` | `FFastArraySerializer`'s delta payload: `FastArrayState` (items with ReplicationID and Key, the array key), `FastArrayShadow` (the last key and ID-to-key map), `write_fast_array_delta`, `write_fast_array_fields` (right after the RepLayout block, or alone: a StatsComponent block has no RepLayout block) and the readers |
| `rpc.h` | RPCs by their `RpcSchema`, both directions: parameters as `FRepLayout::SendPropertiesForRPC` writes them, the field header, `read_client_bunch` for C>S bunches |
| `subobject.h` | Package-map exports (`ExportLink`, `ExportChain`, `pack_exports` into bunches of at most 7621 bits, `ExportAcks`), and content blocks (`write_content_block`, `read_content_blocks`): stably named and dynamic subobjects |
| `move_packed.h` | The parameter of `ServerMovePacked` (plan A6): `read_move_packed` / `write_move_packed` for the `FCharacterServerMovePackedBits` the client sends, its new, pending and old moves (TimeStamp, acceleration, location, control rotation, flags, movement base and mode, AoC's input direction). The layout is from the exe and every captured move reads and writes back to the client's bits |
| `ability_rpc.h` | The ability RPCs of plan A7: `parse_trigger_ability` and `parse_held_ability_cancelled` read `ServerTriggerAbility` and `ServerNotifyHeldAbilityCancelled` from a decoded call (`TriggerAbility`: AbilityGuid, ClientInstanceId, AnimationIndex, InputVector, the targets by GUID and, since the E1 stage 3 fix, the ones named in place as `inline_targets` (an NPC's CollisionCylinder: the client has no GUID for a component the server never exported), ItemData; the cancel's ClientInstanceId), `read_trigger_prefix` reads the first two parameters of a call that did not decode whole, and `client_end_ability` builds the server's `ClientEndAbility(ClientInstanceId)` (reliable). All 54 captured calls of `tests/capture_abilities.h` read and write back to the client's bits |
| `item_codec.h` | The item structs of plan B2, read from the client exe and tested bit for bit against the retail captures: `FItemInstance` (`ItemInstance`: has-item bit, record id, four packed ints, gems, durability, stolen, GUID, creation time, crate guid, the three duplicated members written twice as the exe does, node, lock, persisted stats, cooldowns, custom data, timestamp; a plain instance is 1,096 bits, the empty one 1), `StorageEventContainer` and `FItemStorageEvent` (`ItemStorageEvent`). Every layout, exe address and bound is in the header's top comment. The readers refuse what the exe's writer never produces (a duplicated member whose copies differ, a packed int that is not minimal or is over 32 bits, a UBOOL above 1, the undecoded optional struct of an event) and bound every count (64 gems, 31 persisted stats, 64 cooldowns, 64 custom data entries, 64 target containers, 4,096 slots a container; a count is also checked against the bits left) so a hostile count buys no memory |
| `storage_rpc.h` | The storage RPCs in a typed form: `StoragePayload` (`ClientReceiveStoragePayload`'s `TargetStorage` and `FItemStorageData`: one `StorageSlot` per `MaximumItems`, an empty slot all zeros, a filled slot's `Slot` its index; `storage_payload_params` / `parse_storage_payload`, on the pawn's and on the PlayerController's schema entry) and `storage_event_params` / `parse_storage_event` for the RPCs whose one parameter is an `FItemStorageEvent` (`ClientReceiveStorageEvent`, `ServerRequestStorageEvent`); since plan E1 stage 3 part 2 the loot window's RPCs: `LootPayload` (`ClientReceiveLootStoragePayload`: `LootActor`, the same `FItemStorageData` as a storage payload with `bClientRetrieveOnly` and each slot's loot permission, and `LootAllocationMethods`), `loot_payload_params` / `parse_loot_payload`, `parse_loot_events` (`ServerRequestLootStorageEvents`: the client's type-3 events) and `remove_listener_params` (`ClientRemoveListenerFromStorage`); `test_repl_loot` decodes every loot RPC of retail capture 552 and rebuilds them bit for bit |
| `item_rpc.h` | The client's item-use RPCs of plan B2 in a typed form: `parse_item_used` and `parse_activate_item` read `ServerItemUsed` and `ServerActivateItem` (the same four parameters, on the pawn: `Storage`, `Slot`, `ExpectedItem` (the instance guid the client believes is there) and `RequestedEquipSlot`, `kAnyEquipSlot` 0xFFFFFFFF in all 141 retail equips) from a decoded call into an `ItemUse`. A parameter at its default is not on the wire and reads as that default |
| `hit_codec.h` | The NetSerialize codec of `FSerializedClientHitConfirmations` (plan D2), the parameter of `ClientHitConfirmationsReliable` / `ClientHitConfirmations`, from the exe (0x1459ac190, 0x1463e1870, 0x1463e1da0 ...): an 11-bit hit count, per hit a 15-bit byte length and the `SerializedData` blob, which is an `FHitInst` (four int64, a UBOOL, the instigator and source `FIntrepidNetGUID`s, a 7-bit target count, up to 40 `FHitTargetInst`: the component, two vectors of doubles, up to 10 stat mods, an interrupt list, a UBOOL, a flags byte, up to 10 hit modifiers) and two prediction UBOOLs, padded to a byte. `HitConfirmations` / `HitInstance` / `HitTarget` are the typed values (in `value.h`); `read_struct` / `write_struct` reach it by the struct name. Every one of the 7,115 captured calls (419 retail captures: 11,025 hits, 11,362 targets) reads to its last bit and writes back the same bits. The readers refuse what the exe's writer never produces (a UBOOL above 1, a count above the exe's own clamps, a blob with a whole spare byte or non-zero padding, more than 64 interrupts) and bound every count |
| `hit_rpc.h` | The hit path in a typed form: `parse_hit_request` / `hit_request_params` for the client's `RequestHitVerification_Server` (`HitRequest`: components by GUID or named in place, the three int64, the quantized actor location, the packed yaw and pitch; read with `CodecOptions::inline_objects`: 41 of the 6,955 captured requests name a component in place), `parse_hit_confirmations` / `hit_confirmations_params` and `client_hit_confirmations(pc, hits, reliable, acks, extra_exports)` for the server's answer (E1 stage 3 fix 2: `extra_exports` are the chains of the objects the hits name that the client may hold no GUID for, a target NPC's CollisionCylinder as `component_export` of its actor, exported with the call unless acknowledged; retail exports each target's cylinder in the confirmation's bunch when no earlier bunch did). The older `protocol/wire/hit_confirmation.h` is the first-guess framing (uint16 counts, no targets) and is superseded |
| `leveling.h` | `ULevelingComponent`, the PlayerController's `XPTracker`, typed (plan D1): `DesignDataId` (`FDesignDataObjectId`) and `LevelingState`, which keeps the five fast maps by key (experience and experience debt int64, certifications int64, skill points uint16, the rank list of each skill tree), sets, changes and erases entries the way `MarkItemDirty` does, applies a decoded payload and writes the deltas a connection's shadows say it lacks. The generated schema needs no struct codec: every item of the five maps is a flat list of scalar commands the generic fast-array layer carries. All 443 captured blocks (272 retail captures) read to their last bit and write back the same bits |
| `skill_rpc.h` | The skill-tree and action-bar RPCs in a typed form (plan D1): `ServerPurchaseSkillBuild` (`SkillBuild`: ranks bought or given back; `refund_only()` is a respec), `ClientUpdateSkillOnActionbar`, `ClientReceiveActionbarSlotPayload` (`ActionbarPayload`, the use data and its enums) and `ServerSetActionbarSlotUseData` (and, plan D1 stage 2, `ServerSetUseRangedWeapon`: `parse_set_use_ranged_weapon` and `set_use_ranged_weapon_params`): a parser from a decoded call and a `*_params` builder each, and emitters on a PlayerController for the two server RPCs. A purchase's reply is the AbilityComponent's `KnownAbilities` / `ReplicatedRecordsToLoad` array update, a StatsComponent `CurrentEffects` item, the XPTracker's rank list for the tree and the skill update, all of which read and write back bit for bit |
| `rpc_fields.h` | The small helpers the typed RPC layers above share: a parameter's value by dotted path, an array parameter's element commands and local handles, typed access to a `Value` |
| `block_reader.h` | Plan E1: `load_object_state(schema, data, bits, has_rep_layout)` reads a captured content block (the RepLayout block, then the fast-array fields; an RPC field is refused) into the `ObjectState` the replicator writes from, trying the rotation width (byte or short) and the customization-preset assumption the bits do not say; `default_block_options()` is the stat table's rule (the two extended `FStatInt32InstRep` families). It turns a retail open into a template a server can make an actor from |
| `vendor_codec.h`, `vendor_rpc.h` | Plan E1, the vendor flow: the NetSerialize codecs of `FVendorPurchaseCart` (vendor guid, an int32 count and per item the record, the quantity and two `TOptional<int64>`: 0x146461550, 0x14643ede0, 0x14593e9e0) and `FVendorTransactionCart` (vendor guid, the instance guids, an operation byte: from capture 716), and the typed RPCs: `parse_interact_request` (`ServerInteractWithInteractableComponent`: the component, a counter, the zone), `parse_vendor_purchase`, `parse_vendor_transaction`, `open_vendor_params` / `parse_open_vendor` (`ClientOpenVendor`: `MerchSetId` is the `VendorDef` guid, `Vendor` the NPC). The three purchases of the captures (1, 1 and 6 items) read to their last bit and write back the same bits. **Fix 4** added `FNodeTaxDetails` (`NodeTaxDetails`: the node's int64 guid, an int32 count and per entry an ANSI `FString` tag, the TaxDetails record guid, an int32 flat tax, two floats and two int32 bools; the exe's NetSerialize 0x145b6b4c0 and the captures' bytes), the parameter of the server's `ClientSetNodeTaxTable`, with `parse_node_tax_request` / `node_tax_request_params` (the client's `ServerGetNodeTaxTable(NodeGuid)`) and `parse_node_tax_table` / `node_tax_table_params`: the two tables of the captures (716 and 1536, 78 entries, 54,537 bits each) read to their last bit and write back the same bits |
| `callout_codec.h` | The mounts milestone, stage 1: the NetSerialize codec of `FLocalizedMessage` (`LocalizedMessage`, value.h), the `LocalizedMessage` of a PlayerController callout (`CalloutQueueReplication`'s items, a classic fast array: every command of an item in order): the string table and the key as FStrings and an int32 count of format arguments (nothing for `LookupArgs`), read from the retail captures (the empty message is 96 bits, `Economy` / `UnequipFailCorruption` 336); 533 callouts of the captures decode with it, none has an argument, so a non-zero count is refused. `test_repl_mount` reads the refused summon's callout ("Cannot summon mount here.", captures 876 and 870) and seven messages that fill it |
| `replicator.h` | `Actor` (an `ObjectState` plus `Subobject`s), `ActorShadow` (a connection's `ObjectShadow` per object) and the three emitters: `open_actor`, `update_actor`, `call_rpc` / `call_rpcs`. Each returns an `Emission` (export bunches and payload, plus the GUIDs they name) that maps onto worldd's `Chain` |

```cpp
repl::Actor pc = make_pc();                      // schema, values, open info, subobjects
repl::ActorShadow shadow;                        // what the connection has: nothing
auto open = repl::open_actor(pc, shadow, acks);  // the first chain on the channel
worldd::Chain c{.channel = 3, .open = true, .exports = open->exports, .payload = open->payload};
// later: set values on pc.obj.state, then
auto update = repl::update_actor(pc, shadow, acks);   // nullopt: nothing changed (or *failed)
```

The storage RPCs (plan B2, `storage_rpc.h`) are built from typed values and sent through `call_rpc` like any other:

```cpp
repl::StoragePayload backpack;                         // an empty Backpack: its target, MaximumItems and one default slot each
backpack.target = backpack_guid;  backpack.maximum_items = 70;  backpack.storage.resize(70);
backpack.storage[3].item = item;  backpack.storage[3].slot = 3;    // a filled slot: the instance, and Slot = its index
auto params = repl::storage_payload_params(*pawn_schema.rpc("ClientReceiveStoragePayload"), backpack);
auto call = repl::call_rpc(pawn, nullptr, "ClientReceiveStoragePayload", *params, {}, acks);   // retail sends it to the PlayerController too

repl::ItemStorageEvent add;  add.type = uint8_t(repl::StorageEventType::Add);  add.item = item;
add.targets.push_back({.object = backpack_guid, .slots = {{3, 1, item.guid}}});   // one slot, the quantity, the instance GUID
auto ev = repl::storage_event_params(*pc_schema.rpc("ClientReceiveStorageEvent"), add);
// a request from the client: repl::parse_storage_event(rpc.call) on a decoded ServerRequestStorageEvent
```

Object GUIDs, paths and checksums are the caller's: the actor and its components come from the
per-ServerId allocator (`protocol/server_guid_allocator.h`), names and checksums from the content.
Chains, partial bunches and sequence numbers belong to worldd's `Connection`, never to the replicator.

### Caller contracts

The replicator is a pure function of the actor, the shadow and the export acks; two things stay with
whoever drives it, and getting either wrong loses data without any error.

- **Atomic emission.** `open_actor` and `update_actor` advance the shadow as they emit, and
  `Connection::send_chain` can refuse the chain (window full, wrong channel state). A shadow advanced
  for a chain that was never sent makes the update lost for good: the next one finds nothing changed.
  Emit on a copy and keep the copy only when the chain is accepted:

  ```cpp
  repl::ActorShadow next = shadow;                        // work on a copy
  auto e = repl::update_actor(actor, next, acks, &failed);
  // to_chain: an Emission's exports and payload into a worldd::Chain
  if (e && connection.send_chain(to_chain(channel, false, *e))) shadow = std::move(next);
  // refused: `shadow` is untouched, and the same update is built again later
  ```

  `can_send_chain` asks the same question without queueing. On `*failed` the shadow is as it was.
  `test_worldd_repl_acks` shows the refused update not lost.
- **Export acks.** UE writes an object's export, with its path, again until the client acknowledged it,
  per GUID (`UPackageMapClient::ReceivedAck`). `ExportAcks` is that knowledge, and only the caller can
  fill it: mark `Emission::exported_by_bunch[i]` when `Connection::take_ack_events` reports export part `i`
  of that chain as delivered, never when the chain is queued (a reliable bunch is not known to have
  arrived until its packet is acked; the capture re-sends pkt#78's exports at pkt#107). A lost part marks
  nothing. A marked GUID stays marked; the set only grows (`forget` removes one), bounded by the objects
  the connection ever had exported. `test_worldd_repl_acks` drives the whole loop with a dropped packet.

- **Lost packets** (plan E1 stage 1, fix 2: `send_ledger.h`). The shadow is "what was last SENT", so a lost UNRELIABLE update leaves the client with the
  old value until something changes the property again. UE resends the current value of what a lost packet carried (`FRepLayout::ReceivedNak`; a fast
  array puts the state from before the payload back); `SendLedger` does the same. Per channel: `update_actor(..., &failed, &record)` gives the update's
  `SentRecord` (the handles written, the fast-array items written or removed, a subobject's first block), `ledger.sent(chain, reliable, parts, record,
  shadow)` once the Connection accepted the chain, `ledger.on_event(chain, delivered, shadow)` for each `AckEvent`, `ledger.clear()` when the channel
  closes or opens. A lost unreliable chain makes the shadow forget its handles (the next update writes the CURRENT value in full), gives its fast-array
  items `kUnknownKey` (an item the array lost is a deletion again) and sets `FastArrayShadow::resend` (the array goes out although its key is the shadow's),
  and makes a lost first block of a subobject new again. What a newer chain carried is left alone (the newest chain owns an entry), so an acknowledged value
  is never sent again for an older loss and a value that changed again goes out once, as the newest. A reliable chain is tracked too (it takes entries
  from older unreliable ones) but its lost parts are no NAK: the Connection resends them. **The caller sends unreliably only while
  `reliable_pending()` is false**: a lost reliable bunch is resent later than the unreliable chains sent since and would overwrite them with older values
  (found by `test_worldd_actor_nak`'s soak). The table is bounded (`kMaxRecords`, the oldest evicted as a NAK). worldd's pawn, PlayerController and PlayerState
  chains are all reliable, so a lost one is resent whole by the Connection and needs none of this (`test_worldd_ability_session`'s lossy soak proves
  the client's Stamina follows the runtime's at 10% loss in bursts of 20); `ActorView` uses it for every other actor. `test_repl_send_ledger` tests it on
  its own: lost first and last updates, bursts, the newest value, acknowledged state, adds / changes / removes of fast-array items, a lost new subobject,
  reliable chains, parts, the bound, and a seeded soak of 280 runs.

Things to know when using it:

- Per-connection replication conditions (OwnerOnly, SkipOwner, InitialOnly, ...) are a hook, not a table:
  `ObjectShadow::active` is a `ParentMask` the caller computes from the connection's role, and the writers
  skip inactive properties; when one turns active, what differs from the shadow is sent. The table of which
  property has which condition (`GetLifetimeReplicatedProps` in the exe) is plan A5's.
- `FastArrayState::insert` replicates nothing alone: call `mark_array_dirty` (or `set_array_key`) after
  inserting into an array the client already has.
- A value set and then removed from an array element is sent as the type's zero value (the client keeps
  the old one otherwise); a type with no known zero (FText, some structs) fails the write instead, so set a
  value rather than remove it. An empty array on a first send goes out as count 0, which is harmless.
- `call_rpcs` refuses a mix of reliable and unreliable RPCs (one bunch has one reliability) and a target that
  is not the actor or one of its subobjects.
- Reading is bounded: at most 16,384 array elements per block, 2,048 changed or deleted fast-array items, 1,024
  RPCs in a client bunch, 65,535 characters per string.
- Not supported: deleting a dynamic subobject (the destroy message is neither written nor read);
  `FFastArraySerializer`'s struct-delta item form and the `TOptional` it needs (the quest cache; the array is
  refused rather than written wrong); a fast array property with `ArrayDim > 1`.
- An update of a RepLayout dynamic array is unproven against the original server's capture (it holds only first
  sends of those) but proven against the retail captures: the AbilityComponent's `KnownAbilities` /
  `ReplicatedRecordsToLoad` updates of 32 purchase replies (a count and the new element) read and write back bit for bit
  (`test_repl_leveling`). Unproven: `ObjectShadow::invalidate` (it reproduces the original server's resend of `AuthServerIDReplicated` at
  pkt#107; UE has no known mechanism for it), and the FText parameter of an RPC, which is always sent, an
  empty one too (whether UE leaves an empty one out is unverified).

## Captured test data (`tools/capture/`)

`tests/capture_blocks.h` and `tests/capture_aoc_codecs.h` hold blocks, values, whole actor-open chains,
RPC blocks and client bunches the original server and the retail client sent, as hex with their packet and
bit position. They are generated, not edited:

```
python tools/capture/gen_all.py            # about 45 s; says "unchanged" for each header
python tools/capture/gen_all.py --check    # write nothing, exit 1 when a header would change
```

It needs `dist\Release\replay_bunches.exe`. See `tools/capture/README.md` for what each part cuts and how
to add a block.

Plan E1's: `capture_npc.h` (`python tools/capture/gen_npc.py [--retail-dir DIR] [--check]`, about 60 s): every NPC and gatherable open of capture 957 (330 NPCs of 25 archetypes, 220 gatherables) and the 2,620 updates that followed the NPC opens, as content blocks (hex, bit count, the schema class that reads it), and the nine vendor NPC opens of capture 716; `capture_vendor.h`
(`python tools/capture/gen_vendor.py`): the five vendor RPCs of the client and the one the server answered with (captures 716, 863, 1531, 1536). The server's own copy of the NPC opens it makes actors from is `server/worldd/spawns/area_spawns.h` (`gen_area_spawns.py`, see `tools/capture/README.md`).

Three headers have a generator of their own: `capture_moves.h` (`gen_moves.py`), `capture_abilities.h` (`gen_abilities.py`) and, for the item codecs of plan B2, `capture_items.h`
(`python tools/capture/gen_items.py [--retail-dir DIR] [--check]`, about 90 s): the item RPCs of the retail login, the original server's Bard and the retail client, with the text an independent
Python decoder (`items_wire.py`) read from each. The retail captures are not in the repository: `--retail-dir` (or `AOC_RETAIL_JL`) is the folder of the converted PCAPRepo files
(`NNNN_<name>.jsonl`; the login is `1198_*`). The header is 300 KB, most of it the 125 + 13 events and ten distinct storage payloads as hex.

Two more, for the leveling and hit codecs of plans D1 and D2, built the same way from the retail captures (`--retail-dir`, `dist/Release/replay_bunches`, the exe), about 35 and 15 s: `capture_hits.h`
(`python tools/capture/gen_hits.py [--retail-dir DIR] [--check]`: 41 `ClientHitConfirmations*` calls of the retail server and 12 `RequestHitVerification_Server` of the retail client, picked out of the 7,115 + 6,955 captured calls for every variant
of the layout, and five synthetic ones at the exe's limits, each with the text of `hit_wire.py`, an independent decoder written from the exe's code) and `capture_leveling.h`
(`python tools/capture/gen_leveling.py`: 12 XPTracker blocks picked out of 443, 17 retail purchases, refunds, swaps and respecs each with the client's request and the server's whole answer (97 blocks), 11 action-bar payloads and the 6 slot calls the client sent;
the text comes from the repo's fast-array and RepLayout decoders and `leveling_wire.py`'s hand-written RPC layouts). The mid-session captures name no class: `tools/capture/retail.py` explains how the generators cut them
(the PlayerController is channel 3, a subobject's class is fixed by its GUID in the pick table). The headers are 126 KB and 439 KB; texts over 60,000 characters are arrays of chunks (an MSVC literal is at most 65,535 bytes).

## Tests

`tests/` holds plain executables that print `[ok ]`/`[FAIL]` and exit 0 on
success. To add one, create `tests/test_<name>.cpp` and add `test_<name>` to
the `foreach` list in `tests/CMakeLists.txt`. Build one with
`scripts/build.ps1 -Target test_<name>`; run everything with
`scripts/run_tests.ps1`. Fixtures are read relative to `AOC_REPO_ROOT`
(the repo root), from `src/protocol/tools/`.

`test_repl_move_packed` reads the 640 + 1,843 `ServerMovePacked` parameters of `tests/capture_moves.h` (generated by
`python tools/capture/gen_moves.py` from the e2e capture and the 2026-09-29 run) to their last bit, writes each back to the same
bits, and checks the TimeStamps and locations form a continuous path. `read_move_packed` is given the buffer's size and
refuses an `end` beyond it (it never reads past the buffer); a seeded fuzz (40,000 reads of mutated captures, random containers and
hostile NumBits, each from a heap buffer of exactly the bytes it needs: an address sanitizer build reports an over-read) runs in
the same test.

`test_repl_ability_rpc` (plan A7) reads the 16 + 31 `ServerTriggerAbility` and 4 + 3 `ServerNotifyHeldAbilityCancelled` calls of `tests/capture_abilities.h` (generated by
`python tools/capture/gen_abilities.py` from the e2e capture and the 2026-09-29 run) to their last bit with `decoded` true, writes each back to the
client's bits, and checks what the research found in all 308 calls of 15 captures: `ClientInstanceId` counts every trigger from 0 without a gap,
every cancel names an earlier Sprint or Guard, `ItemData` is "no item" (Slot -1, Location 24), `bPvpForceFlag` is 0; the empty `FItemInstance`
(the whole codec is `test_repl_items`'s), the `InlineObject` form (the flags the form does not define are refused on read as well as on write), and `ClientEndAbility` read back by the schema-driven reader; a seeded fuzz (16,254 captured bunches with bits flipped,
cut short or with a noisy tail, each from a heap buffer of exactly the bytes it needs) checks that nothing crashes and every trigger or cancel that decodes reads back equal from the bits it writes.

The replication tests (`test_generated_schemas`, `test_repl_codecs`, `test_repl_aoc_codecs`, `test_repl_rep_block`,
`test_repl_fast_array`, `test_repl_rpc`, `test_repl_actor_open`) compare bit for bit against
`tests/capture_blocks.h` and `capture_aoc_codecs.h`, which `tools/capture/gen_all.py` regenerates from
`fixtures/replay_data.bin`. `test_worldd_channels` and `test_worldd_repl_acks` (`server/worldd/tests`) run the
replicator on worldd's `Connection`. After changing the schema generator, run `schemagen.py --check`.

`test_repl_item_rpc` (plan B2) reads `ServerItemUsed` and `ServerActivateItem` from four of the retail client's own calls (an equip and an unequip of a chest piece, a two-hander, a focus: 201 bits each) with the pawn's schema entry, parses them (the values the research read), writes them back from the typed form to the same bits,
checks that the two RPCs give the same `ItemUse` and that each parser refuses the other's call, an undecoded call and any other RPC, and fuzzes the payload (every truncation and a seeded run of bit flips).

`test_repl_items` (plan B2) reads every item RPC of `tests/capture_items.h` (generated by `python tools/capture/gen_items.py` from the full retail login `ashesfinalui`, the original server's Bard and
the retail client's own calls) with the PlayerController's, the pawn's or the AbilityComponent's schema entry, through the typed layer and back to the same bits: the login's 125 `ClientReceiveStorageEvent`
and six distinct `ClientReceiveStoragePayload` (Backpack 70 slots, Equipment 83, Quest 100, three resource bags; each sent to the PlayerController and to the pawn several times: 2,342 `FItemInstance`, 1,512 of
them empty), the Bard's 13 events and four payloads (1,217 instances), the client's 31 `ServerRequestStorageEvent` and 15 `ServerTriggerAbility` with an item. Each of the 174 distinct instances is also read alone from its own
bits and written back, and the text of everything read (instances, events, payloads) is compared with what an independent Python decoder (`tools/capture/items_wire.py`, written from the exe's code) made of the same
bits. The fields no retail instance has (gems, custom data, a stolen or crate-carrying item, an enchantment or tempering value, packed ints of more than one byte) are checked against that decoder's own writer on 8
synthetic instances (`kSynthetic`, not retail data). Hostile input: each refusal on its own, every truncation, a hostile count, and a seeded fuzz of 4,888 mutated captures (bit flips, cuts, noisy tails, special
words at random places, each from a heap buffer of exactly the bytes it needs) in which whatever reads writes back to a payload that reads equal and nothing takes long.

`test_repl_hits` (plan D2) reads the 41 captured `ClientHitConfirmationsReliable` / `ClientHitConfirmations` payloads of `tests/capture_hits.h` (generated by `python tools/capture/gen_hits.py`: a sample of the 7,115 captured calls, picked for every variant: reliable and unreliable, 1 to 20 hits, 0 to 9 targets, 0 to 3 stat mods, 0 to 9 hit modifiers, interrupts, the attempted-interrupt flag, the eleven Flags values seen, pvp, an unset Source, each prediction-flag pair) with the PlayerController's schema entry, parses them into `HitConfirmations`, builds the parameters again from the typed form and writes them: the same bits, and every hit alone as a `SerializedData` blob. The 12 captured `RequestHitVerification_Server` (0 to 9 components, one named in place, one naming `Default_Self`) go the same way with the AbilityComponent's entry and `CodecOptions::inline_objects`. What each call holds is compared with the text an independent Python decoder (`tools/capture/hit_wire.py`) made of the same bits, five synthetic calls at the exe's limits (10 stat mods and hit modifiers, 40 targets, a NaN, -0.0, Flags 255) agree with that decoder's writer, and `client_hit_confirmations` reads back through the field loop. Hostile input: each refusal on its own (counts over the exe's clamps, a UBOOL of 2, padding, a spare byte, every truncation) and a seeded fuzz of the 58 payloads (16,298 mutated reads: bit flips, cuts, noise tails and special words, each from a heap buffer of exactly the bytes it needs) in which whatever reads writes back to a payload that reads equal. A one-off sweep, not kept, read all 14,070 captured hit RPCs (7,115 confirmations with 11,025 hits and 11,362 targets, 6,955 requests with 41 components named in place) and wrote each back to the same bits.

`test_repl_leveling` (plan D1) reads 12 XPTracker blocks of `tests/capture_leveling.h` (generated by `python tools/capture/gen_leveling.py`, a sample of the 443 captured ones) with the generated `ULevelingComponent` schema, applies them to a `LevelingState`, writes each back from the state a server would hold (a first send from the typed items themselves, an update from the shadow its base key implies) and compares its text with the Python reader's; 17 retail purchases, refunds, swaps and respecs (the client's `ServerPurchaseSkillBuild` and the server's whole answer, 97 blocks: the AbilityComponent's RepLayout array updates, the StatsComponent's `CurrentEffects`, the XPTracker's tree list, the `ClientUpdateSkillOnActionbar` calls) read, typed and write back bit for bit, with the answer checked against the request (the tree's list holds the ranks bought and not the ones given back, one skill update per purchase and none for a respec); 11 action-bar payloads (1 to 494 slots) and the 6 `ServerSetActionbarSlotUseData` calls the same way; `LevelingState` on its own (entries, deltas against shadows, a second state applying them, erases as deleted IDs); and a seeded fuzz of every captured block and payload (27,443 mutated reads). A one-off sweep, not kept, read all 443 XPTracker blocks, 128 purchase requests, 223 skill updates, 150 action-bar payloads, 6 slot calls, 32 AbilityComponent array updates and 128 CurrentEffects blocks of the retail captures and wrote each back to the same bits. `test_generated_schemas` checks the generated `ULevelingComponent` (fields 2 to 6, the nested rank list, `ServerDemoteCertification`).

`test_repl_mount` (the mounts milestone, stage 1) reads the four mount opens of `tests/capture_mount.h` (`python tools/capture/gen_mount.py`: captures 911, 896, 898 and 871, 36 blocks: the `ANPCCharacter` actor block and its eight subobjects, among them the new `UMountComponent`) to their last bit and writes each back to the same bits; checks the NetGUID pattern (the actor's id A: A+4 ... A+14, the MountComponent A+18, the InteractableComponent A+20), the export names the three kinds of mount component resolve through, the summoning flag, the owner and the stats at the owner's level (MaxHealth 3572 at level 20, 24375 = 4875 x 5 for a Dawnbreaker at 25); reads nine PlayerController callouts (the refused summon's and seven that fill `FLocalizedMessage`) and writes them back (the shadow primed at the delta's base key: they are updates of an array that held earlier items); the codec on its own (sizes, refusals, truncations, non-ASCII text); and a seeded fuzz of the captured blocks.

`test_repl_npc` (plan E1) reads every NPC and gatherable block of `tests/capture_npc.h` with the generated schemas (a RepLayout block, then the field loop: fast arrays and RPC fields) and writes it back from the typed values to the same bits: 330 NPC opens (the research's 323 and seven more whose subobjects the capture never names), the
2,620 NPC updates (ReplicatedMovement and its timestamp, `CurrentMovementInput` alone, the stats' fast arrays, the shared multicast RPC, whose payload has no codec yet: its header and bit count only), the 220 `AGatherableActor` opens with their `UInteractableComponent`, and the 9 vendor NPCs of capture 716; `FPlayerMovementInput` as a value (every direction, a value above 15 refused); and a seeded fuzz of
the blocks. `test_repl_vendor` (plan E1) reads the six captured vendor calls of `tests/capture_vendor.h` with the schema entry of their RPC, parses them, builds the parameters again from the typed form and writes them: the same bits; checks what each holds (the vendor guid is `ClientOpenVendor`'s `MerchSetId`, quantities, the one cart with an optional set); the limits (255 items, a count the bits cannot hold, an optional flag of 2, a negative count); and the truncations of every call and a seeded fuzz of 4,000 mutations.

## Known issues

- `protocol/emit/actor_builder.cpp` reads `probe_v3_subobjects.txt` from the
  current working directory when it opens a subobject, so output depends on
  where the process runs.
- The tests are not registered with CTest; `scripts/run_tests.ps1` is the runner.
