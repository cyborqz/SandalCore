// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/value.h
//
//  Value: one replicated property's value, tagged by what the wire carries for
//  it (the kinds FRepLayout's commands and NetSerialize structs produce):
//
//    bool, signed / unsigned integers (an enum is an unsigned), float, double,
//    FString (UTF-8 here, ANSI or UCS-2 on the wire), FName, FText, an object
//    reference (AoC's 128-bit FIntrepidNetworkGUID), a soft object, a
//    NetSerialize struct in the typed form its codec reads and writes
//    (protocol/repl/codecs.h), or a dynamic array: a vector of element states,
//    each the values of the element's commands keyed by their local handle.
//
//  Equality is what the per-connection shadow compares (rep_block.h): floats
//  and doubles by bit pattern, so a value differs from what was sent exactly
//  when it would put other bits on the wire.
//
//  Quantized values (FVector_NetQuantize*, FRepMovement, FRotator) are held as
//  the integers the wire carries, not as floats: the bits then round trip
//  exactly, and codecs.h has quantize_vector()/dequantize_vector() for the
//  conversion.  AoC's own structs are held the same way, as the tokens of their
//  wire form (CharacterCustomization's deltas, raw float bit patterns; an FItemInstance's
//  fields, floats as floats compared by bits).
// ============================================================================
#pragma once

#include "protocol/emit/intrepid_netguid.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aoc { namespace protocol { namespace repl {

inline bool same_bits(double a, double b) { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }
inline bool same_bits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

/// FName as UPackageMap::SerializeName sends it: a hardcoded (EName) index, or
/// the string and FName::GetNumber() (0 = no number, n = suffix n-1).
struct NameValue {
    bool        hardcoded = false;
    uint32_t    index = 0;     // hardcoded
    std::string text;          // else
    int32_t     number = 0;
    bool operator==(const NameValue&) const = default;
// (C.O.A ~ C-Y-B)

    static NameValue hardcoded_name(uint32_t ename_index) { NameValue n; n.hardcoded = true; n.index = ename_index; return n; }
    static NameValue plain(std::string s, int32_t number = 0) { NameValue n; n.text = std::move(s); n.number = number; return n; }
};

/// FText::SerializeText for the two histories the capture carries: None
/// (0xFF: a culture-invariant string when has_string) and Base (0: namespace,
/// key, source string).  Other histories are not decoded.
struct TextValue {
    uint32_t    flags = 0;
    uint8_t     history = 0xFF;
    bool        has_string = false;   // None
    std::string ns, key, source;      // Base; None keeps its string in `source`
    bool operator==(const TextValue&) const = default;
};

/// FSoftObjectPtr over the net: a path string, or an object reference.
struct SoftObjectValue {
    bool                       is_path = false;
    std::string                path;
    emit::FIntrepidNetworkGUID object;
    bool operator==(const SoftObjectValue&) const = default;
};

// ---- NetSerialize struct values (codecs.h reads and writes each) -----------

/// FVector (three doubles at AoC's net version), FVector3d.
struct Vec3d {
    double x = 0, y = 0, z = 0;
    bool operator==(const Vec3d& o) const { return same_bits(x, o.x) && same_bits(y, o.y) && same_bits(z, o.z); }
};

/// FVector2D / FVector2d (two doubles).
struct Vec2d {
    double x = 0, y = 0;
    bool operator==(const Vec2d& o) const { return same_bits(x, o.x) && same_bits(y, o.y); }
};

/// FRotator as compressed components: the wire integer of pitch, yaw, roll
/// (16 bits from FRotator::SerializeCompressedShort, 8 from SerializeCompressed;
/// a component of 0 costs one bit).
struct Rot {
    uint16_t pitch = 0, yaw = 0, roll = 0;
    bool operator==(const Rot&) const = default;
};

/// UE::Net::WriteQuantizedVector's two forms.  Scaled/Unscaled: integer
/// components (value * scale, or the rounded value when it is too large to
/// scale); the wire width is derived (the widest component's two's-complement
/// width).  RawFloat/RawDouble: full precision, x/y/z hold the IEEE bit
/// patterns.
struct QuantVec {
    enum class Mode : uint8_t { Scaled, Unscaled, RawFloat, RawDouble };
    Mode    mode = Mode::Scaled;
    int64_t x = 0, y = 0, z = 0;
    bool operator==(const QuantVec&) const = default;
};

/// FVector_NetQuantizeNormal: three fixed compressed floats of 16 bits each.
struct Normal16 {
    uint16_t x = 0, y = 0, z = 0;
    bool operator==(const Normal16&) const = default;
};

/// FRepMovement::NetSerialize at AoC's net version (RepMoveOptionalAcceleration):
/// the flag bits for sleep / physics (server frame and handle are flagged by
/// their presence), location, rotation, linear velocity, angular velocity when
/// rep_physics, then the optional acceleration.
struct RepMovement {
    bool                    simulated_physic_sleep = false;
    bool                    rep_physics = false;
    QuantVec                location, linear_velocity, angular_velocity;
    Rot                     rotation;
    std::optional<uint32_t> server_frame;
    std::optional<uint32_t> server_physics_handle;
    std::optional<QuantVec> acceleration;
    bool operator==(const RepMovement&) const = default;
};

/// FCharacterLoadTracker (net_serializers.h): up to 25 flags.
struct LoadTracker {
    std::vector<bool> flags;
    bool operator==(const LoadTracker&) const = default;
};

/// FUniqueNetIdRepl::NetSerialize: the encoding flags (bit 0 encoded, 1 empty,
/// bits 3-7 the type hash) and, by form: V2 (hash 30) the online services byte
/// and bytes; encoded the bytes (hash 31 with the type name first); unencoded
/// the id string (hash 31 with the type name first).
struct UniqueNetId {
    uint8_t              flags = 0;
    uint8_t              services = 0;
    std::string          type;
    std::vector<uint8_t> bytes;
    std::string          id;
    bool operator==(const UniqueNetId&) const = default;
};
// C O A : C_Y_B

/// FDateTime / FTimespan: 64-bit ticks.
struct Ticks {
    int64_t ticks = 0;
    bool operator==(const Ticks&) const = default;
};

/// FGameplayTag as AoC ships it: the tag's FName.
struct GameplayTag {
    NameValue name;
    bool operator==(const GameplayTag&) const = default;
};

/// FGameplayTagContainer: at most 63 tags.
struct GameplayTagContainer {
    std::vector<NameValue> tags;
    bool operator==(const GameplayTagContainer&) const = default;
};

/// FStatInt32InstRep, the item of AoC's stat fast arrays (UAoCStatsComponent
/// StatRepInt32*): its Record, its packed Value, and, only for a Record the
/// client's stat table marks, Base and EquipmentValue (codecs.h).  Every
/// FStatPackedInt32 is a raw 32-bit word (the captured values are float bit
/// patterns: 0x41f00000 is 30.0).
struct StatInt32Inst {
    int64_t  record = 0;
    uint32_t value = 0;
    bool     extended = false;   // Base and EquipmentValue follow on the wire
    uint32_t base = 0, equipment = 0;   // 0 unless extended
    bool operator==(const StatInt32Inst&) const = default;
};

// ---- AoC's own NetSerialize structs (codecs.h: FQuat, FGuidToBoolWrapper,
//      FAlignmentContainer, FAppearanceId, FCharacterCustomizationSaveData) ----

/// FQuat::NetSerialize: X, Y, Z as doubles (AoC's net version is past
/// SerializeDoubleVectorsAsDoubles, the test FVector uses too).  W is not on
/// the wire: the client derives it, sqrt(1 - x^2 - y^2 - z^2), so a value here
/// is what the wire carries, not the quaternion.  quat_to_wire / quat_from_wire
/// (codecs.h) convert as the exe's save and load do.
struct Quat {
    double x = 0, y = 0, z = 0;
    bool operator==(const Quat& o) const { return same_bits(x, o.x) && same_bits(y, o.y) && same_bits(z, o.z); }
};

/// FPlayerMovementInput (ABaseCharacter's CurrentMovementInput, plan E1): the four direction bits of the character's move input, the same four
/// bits as the input direction of ServerMovePacked (move_packed.h; bit 0 x > 0, 1 x < 0, 2 y > 0, 3 y < 0), as a NetSerialize struct of nothing
/// but those four bits.  Every NPC open of the retail captures ends with it (an NPC walking is x > 0: forward), and a 21-bit update of just it
/// is 26 % of the NPC updates.
struct PlayerMovementInput {
    uint8_t bits = 0;   // 0 to 15
    bool operator==(const PlayerMovementInput&) const = default;
    static constexpr uint8_t kForward = 1, kBackward = 2, kRight = 4, kLeft = 8;
};

/// An FGuid as `Ar << Guid` writes it: A, B, C, D as four int32.
struct GuidValue {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    bool operator==(const GuidValue&) const = default;
};

/// FGuidToBoolWrapper (UInteractableComponent's InteractVisibleStateMap /
/// InteractGameplayStateMap items): its TMap<FGuid, bool> in the map's order.
struct GuidToBoolWrapper {
    std::vector<std::pair<GuidValue, bool>> pairs;
    bool operator==(const GuidToBoolWrapper&) const = default;
};

/// FAlignmentContainer (UAlignmentComponent Alignments / InheritedAlignments):
/// AlignmentInstanceMap, a TMap<int64, FGuidsSet> (the set is a TSet<FGuid>),
/// then InstanceTimers, a TMap<uint32, FAlignmentInstanceTimer>.  Both in the
/// maps' order.  The timer keeps its words raw: SetTimeStamp is a double's bit
/// pattern, AlignmentId (FCharacterAlignmentId) two 8-byte words.
struct AlignmentContainer {
    struct Instance {
        uint64_t               key = 0;
        std::vector<GuidValue> guids;
        bool operator==(const Instance&) const = default;
    };
    struct Timer {
        uint32_t key = 0;
        uint64_t set_time_stamp = 0;
        uint64_t alignment_id_a = 0, alignment_id_b = 0;
        bool operator==(const Timer&) const = default;
    };
    std::vector<Instance> instances;
    std::vector<Timer>    timers;
    bool operator==(const AlignmentContainer&) const = default;
};

/// FAppearanceId (the AppearanceIDs of UCharacterAppearanceComponent): the
/// reflection members RecordGuid and TypeId (int64), EnchantmentLevel and
/// ItemEquipSlot (bytes).
struct AppearanceId {
    uint64_t record_guid = 0;
    uint64_t type_id = 0;
    uint8_t  enchantment_level = 0;
    uint8_t  item_equip_slot = 0;
    bool operator==(const AppearanceId&) const = default;
};

/// One socketed gem of an FItemInstance (200 bits on the wire).
struct SocketedGem {
    uint8_t  socket_index = 0;
    uint64_t item_record_id = 0;
    uint64_t socket_record_id = 0;
    uint32_t quality = 0;
    bool     flag = false;      // a UBOOL (32 bits, 0 or 1); what it means is not known
    bool operator==(const SocketedGem&) const = default;
};
// [C O A :: C_Y_B]

/// One persisted stat of an item (a mount's Health and Mana): the stat record and the StatMod value (a float).
struct PersistedStat {
    uint64_t stat_id = 0;
    float    value = 0;
    bool operator==(const PersistedStat& o) const { return stat_id == o.stat_id && same_bits(value, o.value); }
};

/// One persisted cooldown of an item: the cooldown record, when it started (a double) and the charges it saved.
struct PersistedCooldown {
    uint64_t record_guid = 0;
    double   date_time_start = 0;
    uint32_t saved_charges = 0;
    bool operator==(const PersistedCooldown& o) const {
        return record_guid == o.record_guid && same_bits(date_time_start, o.date_time_start) &&
               saved_charges == o.saved_charges;
    }
};

/// One entry of an item's custom data: its UniqueDataType byte and an FString.
struct PersistedCustomData {
    uint8_t     type = 0;
    std::string text;
    bool operator==(const PersistedCustomData&) const = default;
};

/// FItemInstance (AoC's inventory item: the Storage array of ClientReceiveStoragePayload, FItemStorageEvent's ItemData,
/// ServerTriggerAbility's ItemData.ItemInstance ...), NetSerialize 0x146540110 (codecs.h has the layout).  A default
/// instance is the empty item, one 0 bit on the wire; with `has_item` the fields follow in the exe's order.
///
/// The exe serializes the instance GUID, CreationTime and CrateGuid twice and bIsStolen as a 32-bit bool and again as a
/// bit; here each is a single member.  The writer writes both copies equal, as retail does (2,342 instances of the
/// capture agree in every copy), and the reader refuses an instance whose copies differ.  ItemVariationRecordId is not
/// on the wire.  Counts and sizes are bounded (item_codec.h).  Equality compares floats by bit pattern.
struct ItemInstance {
    bool     has_item = false;
    uint64_t record_id = 0;            // ItemRecordId: the item's ItemRecord guid
    uint32_t quantity = 0;
    uint32_t quality = 0;              // an index into the item's QualityCurve (Common 1-5 ... Artifact 101-200)
    uint32_t enchantment_level = 0;
    uint32_t tempering_value = 0;
    std::vector<SocketedGem> gems;
    float    durability = 0;
    bool     stolen = false;
    GuidValue guid;                    // the instance id
    int64_t  creation_time = 0;        // an FDateTime: .NET ticks
    GuidValue crate_guid;
    uint64_t node_guid_purchased_from = 0;
    int64_t  lock_expiration = 0;
    std::vector<PersistedStat>       stats;       // at most 31
    std::vector<PersistedCooldown>   cooldowns;
    std::vector<PersistedCustomData> custom_data;
    double   synchronized_timestamp = 0;   // a double (unix seconds on a mount)

    bool operator==(const ItemInstance& o) const {
        return has_item == o.has_item && record_id == o.record_id && quantity == o.quantity && quality == o.quality &&
               enchantment_level == o.enchantment_level && tempering_value == o.tempering_value && gems == o.gems &&
               same_bits(durability, o.durability) && stolen == o.stolen && guid == o.guid &&
               creation_time == o.creation_time && crate_guid == o.crate_guid &&
               node_guid_purchased_from == o.node_guid_purchased_from && lock_expiration == o.lock_expiration &&
               stats == o.stats && cooldowns == o.cooldowns && custom_data == o.custom_data &&
               same_bits(synchronized_timestamp, o.synchronized_timestamp);
    }
};

/// One entry of a StorageEventContainer: a slot, how many of the item, and the item's instance GUID.
struct StorageEventSlot {
    int32_t   slot = 0;
    int32_t   count = 0;
    GuidValue item_guid;
    bool operator==(const StorageEventSlot&) const = default;
};

/// StorageEventContainer (exe 0x1464614d0): a storage object, the slots the event touches in it and the
/// EItemStorageErrors byte (14, ItemStorage_NumErrors, in every captured event: "no error").
struct StorageEventContainer {
    emit::FIntrepidNetworkGUID     object;
    std::vector<StorageEventSlot>  slots;
    uint8_t                        status = 14;
    bool operator==(const StorageEventContainer&) const = default;
};

/// FItemStorageEvent (NetSerialize 0x145c58f20): what the server tells the PlayerController (ClientReceiveStorageEvent) and
/// what the client asks for (ServerRequestStorageEvent).  `type` is EItemStorageEventType (Add 0, Remove 1, Move 2, Fill
/// 3, Swap 4).  The three extra members are TOptionals the exe serializes after the instigator; none is ever set in a
/// capture, and their meaning is not known.  A further optional struct the exe reads (a UBOOL after ItemData; the research
/// of 2026-10-02 found 9 retail events of delete and chest flows that set it, none in tests/capture_items.h) is not decoded:
/// such an event is refused.
struct ItemStorageEvent {
    StorageEventContainer              move_source;
    ItemInstance                       item;
    uint8_t                            type = 0;
    GuidValue                          instigator;
    std::optional<GuidValue>           extra_guid_a;
    std::optional<uint64_t>            extra_i64;
    std::optional<GuidValue>           extra_guid_b;
    std::vector<StorageEventContainer> targets;
    bool operator==(const ItemStorageEvent&) const = default;
};
// -- Cyb . C | O | A --

/// How a Value holds an ItemStorageEvent: on the heap.  The event is 350 bytes, and every Value of every replicated
/// object (thousands: the stats, the RepLayout values, their shadows) would be as big as the biggest alternative; boxed, a
/// Value stays at the size of an ItemInstance.  Copies are deep, equality compares the events.  Code never names it:
/// Value(ItemStorageEvent) and Value::get_if<ItemStorageEvent>() do.  A moved-from box is empty and must only be
/// destroyed or assigned to.
class EventBox {
public:
    EventBox(ItemStorageEvent e) : p_(std::make_unique<ItemStorageEvent>(std::move(e))) {}
    EventBox(const EventBox& o) : p_(o.p_ ? std::make_unique<ItemStorageEvent>(*o.p_) : nullptr) {}
    EventBox(EventBox&&) noexcept = default;
    EventBox& operator=(const EventBox& o) { p_ = o.p_ ? std::make_unique<ItemStorageEvent>(*o.p_) : nullptr; return *this; }
    EventBox& operator=(EventBox&&) noexcept = default;
    const ItemStorageEvent* get() const { return p_.get(); }
    ItemStorageEvent*       get() { return p_.get(); }
    bool operator==(const EventBox& o) const { return p_ && o.p_ ? *p_ == *o.p_ : !p_ && !o.p_; }
private:
    std::unique_ptr<ItemStorageEvent> p_;
};

/// An object a client RPC parameter names in place instead of by its GUID: the GUID with ObjectId 1, then the export
/// flags (UE's FExportFlags: 1 bHasPath, 2 bNoLoad, 4 bHasNetworkChecksum), with a path the outer's GUID, the object's
/// name (an FString) and, with the checksum flag, the 32-bit network checksum -- what the server writes in an export
/// bunch (ExportLink), here inside the parameter.  The retail client sends it for a target it cannot name by GUID: the
/// pawn's CollisionCylinder (flags 5, checksum 0xb876932f, the pawn as outer) in the Scepter combos of the e2e capture
/// (8 of its 16 triggers).  Read only when CodecOptions::inline_objects says a parameter may be one.
struct InlineObject {
    uint8_t                    flags = 0;
    emit::FIntrepidNetworkGUID outer;       // with the path flag; never itself an inline object
    std::string                path;        // with the path flag
    uint32_t                   checksum = 0;   // with the checksum flag
    bool operator==(const InlineObject&) const = default;
};

// ---- hit confirmations (plan D2: hit_codec.h has the layouts and the exe addresses) --------------------------------

/// One StatModInst of a hit target (what the hit did to a stat): the stat's design-data guid and two floats, compared by bits.
struct HitStatMod {
    int64_t guid = 0;
    float   value = 0;
    float   effective_value = 0;
    bool operator==(const HitStatMod& o) const {
        return guid == o.guid && same_bits(value, o.value) && same_bits(effective_value, o.effective_value);
    }
};

/// One HitModInst: a hit modifier's design-data guid and the object that owns it.
struct HitModInst {
    int64_t                    guid = 0;
    emit::FIntrepidNetworkGUID owner;
    bool operator==(const HitModInst&) const = default;
};

/// One FHitTargetInst, the members the exe replicates (HitComp, ImpactLocation, ImpactNormal, StatMods, Interrupts,
/// bAttemptedInterrupt, Flags, HitModInsts; Dispels, LingeringZoneInsts and AppliedEffects are not on this wire).
struct HitTarget {
    emit::FIntrepidNetworkGUID hit_comp;             // the component that was hit
    Vec3d                      impact_location;      // three doubles
    Vec3d                      impact_normal;        // three doubles (a unit vector in every captured target)
    std::vector<HitStatMod>    stat_mods;            // at most 10
    std::vector<int64_t>       interrupts;           // InterruptInst: the interrupted ability guids
    bool                       attempted_interrupt = false;
    uint8_t                    flags = 0;
    std::vector<HitModInst>    hit_mods;             // at most 10
    bool operator==(const HitTarget&) const = default;
};

/// One FHitInst with the two prediction flags the confirmation wraps it in (one SerializedData blob of an
/// FSerializedClientHitConfirmation).
struct HitInstance {
    int64_t                    inst_guid = 0;
    int64_t                    record_guid = 0;          // the hit's AbilityHitRecord
    int64_t                    source_record_type = 0;
    int64_t                    source_record_guid = 0;   // the ability that made the hit
    bool                       pvp_force = false;
    emit::FIntrepidNetworkGUID instigator;
    emit::FIntrepidNetworkGUID source;
    std::vector<HitTarget>     targets;                  // at most 40
    bool                       predicted_on_autonomous = false;
    bool                       predicted_on_simulated = false;
    bool operator==(const HitInstance&) const = default;
};

/// FSerializedClientHitConfirmations: the parameter of ClientHitConfirmations / ClientHitConfirmationsReliable.
struct HitConfirmations {
    std::vector<HitInstance> hits;                       // at most 1,000
    bool operator==(const HitConfirmations&) const = default;
};

/// One element of FVendorPurchaseCart::ItemCart (plan E1): an item record and a quantity, and two TOptional<int64> (read by the exe's
/// 0x14593e9e0: a 32-bit "set" flag, then the 64-bit value when set) whose meaning is not known: both are unset in 1,034 of the 1,035
/// elements of the retail captures and the one set (capture 1531) holds an item record id.
struct VendorCartItem {
    int64_t                item_record = 0;
    int32_t                quantity = 0;
    std::optional<int64_t> optional_a, optional_b;
    bool operator==(const VendorCartItem&) const = default;
};

/// FVendorPurchaseCart (ServerHandleVendorPurchase): the vendor (a VendorDef guid: ClientOpenVendor's MerchSetId) and the items bought.
/// The exe's NetSerialize (0x146461550): VendorRecordGuid (8 bytes), then ItemCart as an int32 count and each element.
struct VendorPurchaseCart {
    int64_t                     vendor = 0;
    std::vector<VendorCartItem> items;     // at most 255 on the wire here (the server refuses more anyway)
    bool operator==(const VendorPurchaseCart&) const = default;
};

/// FVendorTransactionCart (ServerHandleVendorTransaction, the sale of items to a vendor and the buy-back): the vendor, the instance guids of
/// the items (four int32 each) and the operation (a byte, EVendorTransactionOperation).
struct VendorTransactionCart {
    int64_t                vendor = 0;
    std::vector<GuidValue> instances;
    uint8_t                operation = 0;
    bool operator==(const VendorTransactionCart&) const = default;
};

/// One entry of FNodeTaxDetails (plan E1, fix 4): the tax of one gameplay tag ("NodeTax.GoodsTax.MarketplaceTax") at one node.  Every entry of
/// the retail captures has the same shape: the tag as an ANSI FString, the TaxDetails record the numbers come from (a design-data guid: the
/// record "TaxDefaultValue" in all 78 entries of captures 716 and 1536), a flat amount (always 0), the current percent and the owning node's
/// percent (floats), and two bools (an int32 each).  The two bools are 1 on the entries a node's owners changed and 0 on its defaults; their
/// exact names are not known.
struct NodeTaxEntry {
    std::string tag;
    int64_t     details = 0;
    int32_t     flat_tax = 0;
    float       tax_percent = 0.0f;
    float       owning_node_percent = 0.0f;
    bool        flag_a = false;
    bool        flag_b = false;
    bool operator==(const NodeTaxEntry&) const = default;
};

/// FNodeTaxDetails (ClientSetNodeTaxTable's parameter, UVendorMenu::HandleReceiveNodeTaxDetails reads it): the node's guid (the one the client
/// asked for in ServerGetNodeTaxTable) and its tax table.  The exe's NetSerialize (0x145b6b4c0): 8 bytes of NodeGuid, then the TMap of tag
/// to entry as an int32 count and each pair.
struct NodeTaxDetails {
    int64_t                   node = 0;
    std::vector<NodeTaxEntry> entries;
    bool operator==(const NodeTaxDetails&) const = default;
};

/// FLocalizedMessage (the mounts milestone, stage 1): a message the client looks up in a string table, the `LocalizedMessage` of a PlayerController
/// callout (`CalloutQueueReplication.Items.Value.LocalizedMessage`: a toast such as "NCS" / "Quest_Toast_Started", a refusal such as "Economy" /
/// "UnequipFailCorruption").  On the wire (callout_codec.h) it is the table's id and the key as FStrings and the format arguments as an int32 count;
/// every captured message has none, so only the empty argument list is modelled (a message with arguments is refused by the codec).  Both strings are empty
/// in a callout whose text is its plain `Message` ("Cannot summon mount here.", captures 876 and 870: all 96 bits of the struct are 0).
struct LocalizedMessage {
    std::string string_table_id;
    std::string key;
    bool operator==(const LocalizedMessage&) const = default;
};

// FCharacterCustomizationSaveData::NetSerialize sends the struct as a delta
// against a default (the preset's values) that never reaches the wire, so the
// value here is the wire's tokens: a field that was not sent is absent
// (std::nullopt / not in `deltas`) and the client takes its default.  Floats
// and doubles are held as their bit patterns, so nothing depends on what a
// default is.
// [C O A x C | Y | B]

/// The 30 scalar deltas in wire order (member names of the exe's reflection
/// record).  Width: 32 bits (a float or an int) or 64 (an int64 id or color).
enum class CustomizationField : uint8_t {
    SkinColorHue, SkinColorPigmentation, NormalDetailStrength01, NormalDetailStrength02, SkinSet,
    EyeColor, EyeShape, ScleraShape, Eyebrows, HeadHair, HeadHairRootColor, HeadHairTipColor,
    HeadHairLength, HeadHairContrast, HeadHairGradient, FacialHairLip, FacialHairChin, FacialHairCheek,
    FacialHairLipLength, FacialHairChinLength, FacialHairCheekLength, EyelashLength,
    FacialHairRootColor, FacialHairTipColor, FacialHairContrast, FacialHairGradient,
    RacialHorns, RacialHornsLength, NailColor, NailOpacity,
};
inline constexpr size_t kCustomizationFieldCount = 30;

/// One FCharacterCustomizationDecalData, the fields in wire order (floats as bits).
struct CustomizationDecal {
    uint64_t guid = 0;
    uint32_t strength = 0, scale_u = 0, scale_v = 0;
    uint8_t  slot_index = 0;
    uint64_t merge_section_material_base = 0;
    uint32_t offset_u = 0, offset_v = 0, angle = 0;
    uint64_t color = 0;
    uint32_t discoloration = 0;
    bool operator==(const CustomizationDecal&) const = default;
};

/// One FCharacterDecalBlendGroup.
struct CustomizationBlendGroup {
    uint64_t decal_appearance_guid = 0, color_guid = 0;
    uint32_t intensity = 0;   // float bits
    bool operator==(const CustomizationBlendGroup&) const = default;
};

/// One SectionsValues pair: the section enum byte and its FVector.
struct CustomizationSection {
    uint8_t key = 0;
    Vec3d   value;
    bool operator==(const CustomizationSection&) const = default;
};

/// One FaceMorphWeightMaps pair: a morph target's FName and its weight (float bits).
struct CustomizationMorph {
    NameValue name;
    uint32_t  weight = 0;
    bool operator==(const CustomizationMorph&) const = default;
};

struct CharacterCustomization {
    std::optional<uint64_t> preset_guid;           // PresetGuid, sent when non-zero
    std::optional<uint32_t> random_seed;           // RandomSeed, sent when non-zero
    /// Race and Gender bytes: on the wire exactly when the client has no preset
    /// for PresetGuid (CodecOptions::customization_preset_known).
    struct RaceGender {
        uint8_t race = 0, gender = 0;
        bool operator==(const RaceGender&) const = default;
    };
    std::optional<RaceGender> race_gender;
    std::map<CustomizationField, uint64_t> deltas; // by CustomizationField: the bits of the value sent
    /// DecalData / DecalBlendGroups: nullopt = unchanged (one bit); else the whole
    /// array's Num, each element sent (its own delta bit set) or nullopt (bit 0).
    std::optional<std::vector<std::optional<CustomizationDecal>>>     decals;
    std::optional<std::vector<std::optional<CustomizationBlendGroup>>> blend_groups;
    std::optional<bool> helmet_visible, cape_visible;
    std::optional<std::vector<CustomizationSection>> sections_values;
    std::optional<std::vector<CustomizationMorph>>   face_morph_weights;
    bool operator==(const CharacterCustomization&) const = default;
};

// ---- Value -----------------------------------------------------------------

struct ElementState;

/// A dynamic array: one state per element (its size is the count on the wire).
struct ArrayValue {
    std::vector<ElementState> elements;
    bool operator==(const ArrayValue&) const;
};

enum class ValueKind : uint8_t {
    None, Bool, Int, UInt, Float, Double, String, Name, Text, Object, SoftObject, Struct, Array,
};

class Value {
public:
    using Storage = std::variant<
        std::monostate, bool, int64_t, uint64_t, float, double, std::string,
        NameValue, TextValue, emit::FIntrepidNetworkGUID, SoftObjectValue, ArrayValue,
        Vec3d, Vec2d, Rot, QuantVec, Normal16, RepMovement, LoadTracker, UniqueNetId, Ticks,
        GameplayTag, GameplayTagContainer, StatInt32Inst,
        Quat, GuidToBoolWrapper, AlignmentContainer, AppearanceId, CharacterCustomization, ItemInstance, InlineObject,
        EventBox, HitConfirmations, PlayerMovementInput, VendorPurchaseCart, VendorTransactionCart, NodeTaxDetails, LocalizedMessage>;

    Value() = default;
// built by C | Y | B with C | O | A

    /// Any non-scalar alternative converts implicitly (a struct, name, text,
    /// object, soft object or array); scalars have named constructors.
    template <class T>
        requires(!std::is_arithmetic_v<std::remove_cvref_t<T>> && !std::is_pointer_v<std::remove_cvref_t<T>> &&
                 !std::is_same_v<std::remove_cvref_t<T>, Value> && std::is_constructible_v<Storage, T &&>)
    Value(T&& v) : v_(std::forward<T>(v)) {}

    static Value boolean(bool b)       { Value v; v.v_ = b; return v; }
    static Value i64(int64_t i)        { Value v; v.v_ = i; return v; }
    static Value u64(uint64_t u)       { Value v; v.v_ = u; return v; }
    static Value f32(float f)          { Value v; v.v_ = f; return v; }
    static Value f64(double d)         { Value v; v.v_ = d; return v; }
    static Value str(std::string s)    { Value v; v.v_ = std::move(s); return v; }

    ValueKind kind() const;

    template <class T> const T* get_if() const {
        if constexpr (std::is_same_v<T, ItemStorageEvent>) {
            const EventBox* b = std::get_if<EventBox>(&v_);
            return b ? b->get() : nullptr;
        } else {
            return std::get_if<T>(&v_);
        }
    }
    template <class T> T* get_if() {
        if constexpr (std::is_same_v<T, ItemStorageEvent>) {
            EventBox* b = std::get_if<EventBox>(&v_);
            return b ? b->get() : nullptr;
        } else {
            return std::get_if<T>(&v_);
        }
    }

    bool operator==(const Value& other) const;

private:
    Storage v_;
};

/// The values of one array element's commands, keyed by local handle (1-based
/// position in the array's element commands).
struct ElementState {
    std::map<uint16_t, Value> values;
    bool operator==(const ElementState& o) const { return values == o.values; }
};

}}}  // namespace aoc::protocol::repl
