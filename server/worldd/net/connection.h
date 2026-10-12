// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  net/connection.h
//
//  One client connection after the handshake (architecture spec §3.2
//  net/Connection): packet acks, every channel's reliable sequence numbers,
//  resending reliable bunches from lost packets, delivering the client's
//  reliable bunches in order (and each once), reassembling reliable partial
//  bunches, keep-alives, and the idle timeout.
//
//  Single-threaded: the network thread owns it.
//
//  Reliable sequences use UE5's MAX_CHSEQUENCE of 1024: each channel starts
//  at initial packet sequence % 1024 (UE5 InitSequence), so the first bunch
//  the server sends on channel 0 is 954 when its initial sequence is 14265,
//  as in the capture.  The Connection owns every channel's counter: emitters
//  never pick a chSeq.  Not yet here (later plans): receiving unreliable
//  partial bunches, bandwidth pacing.
//
//  Actor channels: chains, partial bunches, open and close
//  -------------------------------------------------------
//  What the original server does, established from fixtures/replay_data.bin
//  (452,536 server bunches in 29,010 packets, every header decoded, the
//  partial and export flags included) and UE 5.6 UChannel::SendBunch,
//  UPackageMapClient::ExportNetGUID and UNetConnection::SendRawBunch:
//
//  A chain is one logical bunch as the emitter sees it: export bunches (the
//  package map's NetGUID exports, bHasPackageMapExports=1), then the content.
//    * Every export bunch is a part of its own, in front of the content, and
//      at most kMaxSingleBunchBits (7621) bits; an export section too big for
//      one is several export parts: ch 14 pkt#62/#63, 7329 + 961 bits, then
//      6804 bits of content (9 chains in the capture have two export parts).
//      No non-partial bunch has exports, no final part has them, and exports
//      are always in front of the content.
//    * A chain with exports is never one part, even a small one: ch 3
//      pkt#22 chSeq 954/955 is the PlayerController open (3545 bits of
//      exports alone, then 1314 bits of SerializeNewActor and content), and
//      chSeq 956/957 the ClientSetHUD RPC (873 bits of BP_AOCHUD_C export,
//      then the 173-bit RPC).  Of the 830 reliable multi-part chains, 768
//      start with an export part; the other 62 are content only.
//    * Content of at most 7621 bits goes out whole, not partial: the largest
//      such bunch in the capture is 7605 bits (ch 3 pkt#7981, chSeq 149);
//      ch 3 pkt#23 chSeq 958 (3550 bits), pkt#77 chSeq 965 (2537) and the
//      2422-bit open of ch 21 pkt#8535 are ordinary ones.
//      More is cut into parts of exactly 7616 bits (952 bytes), the last
//      part taking the rest (37..7602 bits).  All 324 non-final content
//      parts in the capture are 7616 bits.  7621 is UE's
//      MAX_SINGLE_BUNCH_SIZE_BITS = 8224 (AoC's MaxPacket 1028 * 8) - 256
//      (MAX_BUNCH_HEADER_BITS) - 1 (trailer) - 308 (MAX_PACKET_HEADER_BITS:
//      32 + 256 history + 1 + 10 + 1 + 8) - 38 (packet handler: magic,
//      session, client, handshake bit); 7616 is that rounded down to bytes.
//      Between 7617 and 7621 bits the capture has no example (unproven).
//      ch 6 pkt#28..#45 is the GameState open: 3473 bits of exports, then
//      123,841 bits of content in 16 x 7616 + 1985 (18 parts, chSeq
//      954..971); ch 3 pkt#72..#76 (chSeq 960..964) is 1009 bits of exports
//      and 25,833 bits of content in 3 x 7616 + 2985.
//    * Flags: with more than one part every part has bPartial; bPartialInitial
//      is on the first part only and bPartialFinal on the last only.  bOpen is
//      on the first part only (752 opens on actor channels, 748 with exports;
//      a content-only open is ch 21 pkt#15976/#15977, 7616 + 5354 bits, or
//      whole: pkt#8535).  bControl is bOpen || bClose (0 exceptions).
//      AoC's extra partial flag (bPartialCustomExportsFinal, UE's NetToken
//      export bit) is 0 in every server bunch, as are bIsReplicationPaused
//      and bHasMustBeMappedGUIDs: never set here.  A chain is reliable or
//      unreliable as a whole: ch 3 pkt#149 is an unreliable chain, 553 bits of
//      exports then 1372, no chSeq, no channel name, a 30-bit header.  An open
//      is always reliable (752 of 752).
//    * chSeq: one per part, consecutive on the channel (954, 955, 956, ...).
//      The counter is the channel's and survives a close: ch 21 opens with
//      954/955 (pkt#100/#101), closes with 956 (pkt#8332), opens again with
//      957 (#8535), closes with 958 (#8539), opens with 959/960 (#15976/77)
//      and closes with 961 (#15979).
//    * Channel name: hardcoded EName 102 (NAME_Actor) in the header of every
//      reliable or open bunch of an actor channel, on every part (2299 of
//      2299); channel 0 uses EName 255.
//    * Close: a reliable bunch of 0 bits of its own, never partial:
//      bControl=1, bClose=1, bOpen=0, a close reason, chSeq, EName 102.
//      195 closes: reason 1 (Dormancy) 179 times, 3 (Relevancy) 15 times,
//      0 (Destroyed) once (EChannelCloseReason).
//    * Packing: parts fill packets in order and share a packet when they fit:
//      pkt#22 carries chSeq 954..957 (3545 + 1314 + 873 + 173 bits), pkt#63
//      the second export part and the content of ch 14; a 7616-bit part fills
//      a packet by itself (pkt#29..#44; the last part of that chain shares
//      pkt#45 with the next channel's export part).  Packets are filled up to
//      the 1028 bytes (up to 7960 bits of bunch data in pkt#296).
//
//  The client reassembles a chain by chSeq; only the content parts go into
//  the reassembled bunch, each export part is read on its own on arrival
//  (UChannel::ReceivedRawBunch), and the reassembled bunch may be at most
//  kMaxPartialBits.
//
//  A chain may have at most kMaxUnackedReliable (256) parts: one that needs
//  more is refused and can never be sent, however empty the window is.  The
//  largest content allowed (kMaxPartialBits, 524,288 bits) is only 69 parts,
//  so only a very long export list can reach the limit.  UE refuses it the same way ("Reliable
//  partial bunch overflows reliable buffer", UChannel::SendBunch; there the
//  limit is 255).  The emitter must keep its chains below it.
//
//  Closing: a channel index stays taken until its close is acknowledged
//  ---------------------------------------------------------------------
//  close_channel queues the close; the channel is closed for sending at once
//  but its index is not free: UE keeps a closing channel in its table until
//  the close bunch is acked (UChannel::Close, UNetConnection::
//  GetFreeChannelIndex, freed in UChannel::ReceivedAcks).  Opening the index
//  sooner is a race on the client: if the close's packet is lost and the open
//  of the same index arrives first, the resent close then runs CleanUp on the
//  new channel and the actor never appears.  So send_chain refuses an open on
//  a closing index (channel_closing) until the packet carrying the close has
//  been acked; a lost close is sent again (same chSeq) first.
//
//  Acks of a chain's parts
//  -----------------------
//  The original server exports a GUID's path again until the client
//  acknowledged it, per GUID, when the packet that carried the export is acked
//  (UPackageMapClient::ReceivedAck / ReceivedNak, PackageMapClient.cpp:2411);
//  queueing the export, even on a reliable channel, proves nothing (the capture
//  agrees: pkt#78's exports come again at pkt#107, 48 ms later).  The caller
//  (the replicator's ExportAcks) therefore needs to know which packets arrived:
//    * send_chain returns the chain's ChainId;
//    * take_ack_events returns an AckEvent for each part of each chain when the
//      packet that carried it is acked or lost: {chain, channel, part,
//      export_part, delivered}.  Parts are numbered in the order they go out:
//      part i < Chain::exports.size() is exports[i], the rest are the content.
//    * delivered: the client acknowledged the packet.  A reliable part's final
//      state: the client has it, in order.
//    * not delivered: the packet was lost.  A reliable part goes out again in
//      a new packet (same chSeq) and its event follows when that one is acked;
//      an unreliable part is never sent again, so a lost one is final.
//  The caller marks ExportAcks for the GUIDs of export bunch i
//  (Emission::exported_by_bunch[i]) when the event for part i says delivered,
//  as UE marks a GUID when its export's packet is acked; Emission::exported
//  lists all the chain's export GUIDs together, for a caller that waits until
//  every export part is delivered (coarser, equally safe).
//  Events are kept until taken, at most kMaxAckEvents; take them after every
//  receive().
//
//  Unreliable data: what cannot be sent is dropped
//  -----------------------------------------------
//  Reliable bunches wait in the queue until the packet window (256 packets, see
//  PacketNotify) has room, and their number is bounded by the channels' windows
//  (kMaxUnackedReliable).  Unreliable bunches have no such bound: while the
//  window is full (a client that stopped acknowledging) every unreliable send
//  would pile up.  UE drops unreliable data it cannot send; so does flush():
//  after it has sent what it can, no more than kMaxQueuedUnreliable unreliable
//  bunches stay queued, and it drops the oldest first (stale data goes, the
//  newest stays).  A part of an unreliable chain takes the rest of its chain
//  with it (the other parts are of no use alone).  Each dropped part is reported
//  to take_ack_events as lost (delivered = false), as a part whose packet was
//  lost is: unreliable parts are final.  Reliable bunches are never dropped.
//
//  Atomic emission: check before you commit
//  ----------------------------------------
//  The replicator's open_actor / update_actor advance the shadow (what the
//  client has) as they emit.  send_chain may refuse the chain (the window is
//  full, the channel is not in the state the chain needs): the shadow must then
//  not have moved, or the update is lost for good (the next update finds
//  nothing changed).  The contract is
//      ActorShadow next = shadow;                     // work on a copy
//      auto e = update_actor(actor, next, acks);      // emit on the copy
//      Chain chain = <the emission>;
//      if (connection.send_chain(chain)) shadow = std::move(next);
//      // refused: keep `shadow`; the same update is built again later
//  can_send_chain(chain) answers the same question as send_chain without
//  queueing, for a caller that wants to know before it builds more.
// ============================================================================
#pragma once
// ~ C.Y.B - C O A ~

#include "protocol/conn/packet_framing.h"
#include "protocol/conn/packet_notify.h"
#include "protocol/emit/bunch_writer.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aoc { namespace worldd {

using Clock = std::chrono::steady_clock;

struct ConnectionConfig {
    protocol::conn::OuterHeader outer;
    protocol::conn::CustomField custom_field{};
    uint16_t server_initial_seq = 0;  // first packet sequence we send
    uint16_t client_initial_seq = 0;  // first packet sequence the client sends
};

/// One logical bunch on an actor channel, before it is split into parts (see
/// the chain rules above).  The Connection sets every part's flags and
/// sequence.
struct Chain {
    uint32_t channel = 0;  // an actor channel; channel 0 uses Connection::send
    bool open = false;     // opens the channel (the first chain on it): reliable
    bool reliable = true;
    /// The package map's export bunches, each a whole bunch payload
    /// (bHasRepLayoutExport, count, objects) of at most kMaxSingleBunchBits;
    /// each goes out as a part of its own, in front of the content.
    std::vector<protocol::emit::BunchWriter> exports;
    /// The content: SerializeNewActor and the content blocks, an RPC, ...
    protocol::emit::BunchWriter payload;
};

/// Names an accepted chain in AckEvents: send_chain returns it, 0 is "refused".
using ChainId = uint64_t;

/// What became of one part of a chain (see "Acks of a chain's parts" above).
struct AckEvent {
    ChainId chain = 0;
    uint32_t channel = 0;
    uint32_t part = 0;          // in sending order: the exports first, then the content
    bool export_part = false;   // the part is an export bunch (bHasPackageMapExports)
    bool delivered = false;     // true: its packet was acked; false: lost (a reliable part is sent again)
};

/// EChannelCloseReason as UE 5.6 has it (the captures use 0, 1, 3 and, since plan E1, 5: 16,699 Dormancy closes, 1,713 Relevancy, 96 Destroyed
/// and 26 Disconnect, the last one a neighbouring server's GameState when the player crosses a border).
enum CloseReason : uint32_t {
    kCloseDestroyed = 0,
    kCloseDormancy = 1,
    kCloseLevelUnloaded = 2,
    kCloseRelevancy = 3,
    kCloseTearOff = 4,
    kCloseDisconnect = 5,
};

/// A bunch from the client, delivered in order and reassembled.
struct InBunch {
    uint32_t channel = 0;
    bool reliable = false;
    bool control = false;
    bool open = false;
    bool close = false;
    uint32_t close_reason = 0;
    std::vector<uint8_t> data;  // the payload, from bit 0
    size_t bits = 0;
};

class Connection {
public:
    static constexpr std::chrono::milliseconds kAckDelay{50};
    static constexpr std::chrono::milliseconds kKeepAliveInterval{200};
    /// Before NMT_Join the client is loading the map and may be silent a while.
    static constexpr std::chrono::seconds kLoginTimeout{120};
    static constexpr std::chrono::seconds kPlayTimeout{60};
    /// Unacknowledged reliable bunches per channel (UE5 RELIABLE_BUFFER).
    static constexpr size_t kMaxUnackedReliable = 256;
    /// Early reliable bunches held for the client, over all channels; one
    /// more fails the connection (UE5: "Reliable buffer overflow").
    static constexpr size_t kMaxEarlyBunches = 256;
    /// The largest reliable bunch reassembled from parts (UE5
    /// NetMaxConstructedPartialBunchSizeBytes); a bigger one fails the connection.
    static constexpr size_t kMaxPartialBits = 64 * 1024 * 8;
    static constexpr int kChSeqSpace = 1024;
    /// Room a bunch may take: a packet's worst-case headers take ~400 bits.
    static constexpr size_t kMaxBunchBits = protocol::conn::kMaxPacketBytes * 8 - 400;
    /// UE's MAX_SINGLE_BUNCH_SIZE_BITS on AoC's connection (derivation above):
    /// content up to this goes out whole, more is split; an export bunch may
    /// not be bigger.
    static constexpr size_t kMaxSingleBunchBits = 7621;
    /// The bits of every part but the last of a split chain (a whole number
    /// of bytes: the client requires it of all parts but the last).
    static constexpr size_t kMaxPartialBunchBits = 7616;
    /// AckEvents held for the caller.
    static constexpr size_t kMaxAckEvents = 1 << 16;
    /// Unreliable bunches kept queued while the packet window is full (see "Unreliable data" above): UE drops
    /// unreliable data it cannot send, and so does flush(), the oldest first.
    static constexpr size_t kMaxQueuedUnreliable = 256;
    /// The most parts a reassembled partial bunch may have: a sender's reliable
    /// window (UE: RELIABLE_BUFFER) holds no more, so more is a peer flooding
    /// empty parts; one more fails the connection.
    static constexpr size_t kMaxPartialParts = kMaxUnackedReliable;
    /// The hardcoded channel name of an actor channel (EName 102, NAME_Actor).
    static constexpr uint32_t kActorChannelName = 102;
    /// A close reason is written with SerializeInt(reason, 15): 0..14.
    static constexpr uint32_t kCloseReasonSpace = 15;

    Connection(const ConnectionConfig& cfg, Clock::time_point now);

    /// Processes one datagram from the client; returns the bunches it
    /// completes, in order.  Stale, duplicate and malformed packets return
    /// nothing.
    std::vector<InBunch> receive(const uint8_t* data, size_t len, Clock::time_point now);
    std::vector<InBunch> receive(const std::vector<uint8_t>& datagram, Clock::time_point now) {
        return receive(datagram.data(), datagram.size(), now);
    }

    /// Queues a bunch.  For a reliable one the connection sets the channel
    /// sequence and sends it again until the client acknowledges it.  False
    /// if the bunch does not fit in one packet or the channel already has
    /// kMaxUnackedReliable bunches in flight.  This is the one-bunch path for
    /// the control channel; actor channels use send_chain and close_channel.
    bool send(protocol::conn::BunchHeader header, const protocol::emit::BunchWriter& payload);
// [C_Y_B :: CoA]

    /// Queues a chain on an actor channel, split into partial bunches as the
    /// original server does (rules above): each export bunch a part, then the
    /// content whole or in parts of kMaxPartialBunchBits; each part gets the
    /// channel's next reliable sequence (or none, for an unreliable chain).
    /// Returns the chain's id (for AckEvents), or 0 when it refuses the chain
    /// whole, with nothing queued:
    ///   - the channel is 0, or is open and the chain opens it, or is not open
    ///     and it does not (a channel is open from its opening chain to
    ///     close_channel), or an open chain is not reliable;
    ///   - the chain opens a channel whose close has not been acknowledged yet
    ///     (channel_closing);
    ///   - an export bunch exceeds kMaxSingleBunchBits, or the content exceeds
    ///     kMaxPartialBits (the client would fail the connection);
    ///   - a reliable chain's parts would take the channel past
    ///     kMaxUnackedReliable bunches in flight; a chain of more parts than
    ///     that never fits, however empty the window.
    /// Accepting a chain is the commit point of an emission: see "Atomic
    /// emission" above for the caller's side.
    ChainId send_chain(const Chain& chain);

    /// Whether send_chain would accept `chain` now: the same checks, nothing
    /// queued or changed (and nothing logged).  A send_chain directly after a
    /// true answer accepts.
    bool can_send_chain(const Chain& chain) const;

    /// The parts `chain` goes out in, which is the number of AckEvents it gets: each export bunch, then the content whole or in parts of
    /// kMaxPartialBunchBits (what plan_chain counts).
    static size_t part_count(const Chain& chain);

    /// Queues the close of an open actor channel: a reliable bunch of 0 bits
    /// with bClose and `reason` (a CloseReason).  The channel is closed for
    /// sending at once, but its index can be opened again only after the
    /// packet carrying the close is acked (channel_closing); the sequence
    /// then goes on.  False when the channel is not open or the window is full.
    bool close_channel(uint32_t channel, uint32_t reason = kCloseDestroyed);

    /// True from an accepted opening chain on `channel` until close_channel.
    bool channel_open(uint32_t channel) const { return open_channels_.count(channel) != 0; }

    /// True from close_channel until the client acknowledges the close: the
    /// index cannot be opened meanwhile (UE frees a closing channel's slot in
    /// ReceivedAcks).
    bool channel_closing(uint32_t channel) const { return closing_.count(channel) != 0; }

    /// The acks and losses of chain parts since the last call, oldest first
    /// (see "Acks of a chain's parts" above).  At most kMaxAckEvents are kept:
    /// beyond that the oldest are dropped, which only makes a caller send an
    /// export once more.
    std::vector<AckEvent> take_ack_events();

    /// Queues closing channel 0: the client drops the connection.
    bool send_close(uint32_t reason = 0);

    /// Datagrams to send now: queued bunches, resends, or a keep-alive that
    /// carries our acks.
    std::vector<std::vector<uint8_t>> flush(Clock::time_point now);

    bool timed_out(Clock::time_point now) const;
    bool closed_by_client() const { return closed_by_client_; }
    /// True once the client broke the protocol (too many early reliable
    /// bunches, or a partial bunch too big): the endpoint drops it.
    bool failed() const { return failed_; }
    /// Switches to the play timeout (after NMT_Join).
    void set_playing() { playing_ = true; }

    size_t unacked_reliable(uint32_t channel) const;
    /// Unreliable bunches (parts of unreliable chains included) queued and not sent yet.
    size_t queued_unreliable() const;
    /// Unreliable bunches dropped for want of room (see "Unreliable data" above), in all.
    size_t unreliable_dropped() const { return unreliable_dropped_; }
    const protocol::conn::PacketNotify& notify() const { return notify_; }
    /// Packets sent and not yet acknowledged (the packet window is 256), and bunches queued that no packet carries yet: what a sender that must not
    /// crowd out the player's own data (the actor system) looks at before it adds more.
    size_t packets_in_flight() const { return static_cast<uint16_t>(notify_.out_seq() - notify_.out_ack_seq()) & 0x3FFF; }
    size_t queued_bunches() const { return outgoing_.size(); }

private:
    struct OutBunch {
        uint32_t channel = 0;
        bool reliable = false;
        std::vector<uint8_t> data;  // the whole bunch, header included
        size_t bits = 0;
        bool acked = false;
        bool queued = false;
        // What the acks of the packet carrying it mean (see AckEvent):
        ChainId chain = 0;          // 0: not a part of a chain
        uint32_t part = 0;
        bool export_part = false;
        bool closes = false;        // the close of its channel: acked, the index is free
    };
    using OutPtr = std::shared_ptr<OutBunch>;

    /// Which part of which chain a queued bunch is.
    struct PartTag {
        ChainId chain = 0;
        uint32_t part = 0;
        bool export_part = false;
    };

    /// A reliable partial bunch being reassembled.
    struct Partial {
        InBunch bunch;
        size_t parts = 0;
    };
// CoA + C-Y-B

    /// Whether `chain` can be sent now, and the parts it makes; `log` says why not.
    bool plan_chain(const Chain& chain, size_t* parts, size_t* content_parts, bool log) const;
    void note_ack(const OutBunch& ob, bool delivered);

    /// A received bunch with its partial flags, before reassembly.
    struct Queued {
        InBunch bunch;
        bool partial = false;
        bool partial_initial = false;
        bool partial_final = false;
    };

    void on_packet_ack(uint16_t seq, bool delivered);
    /// After the send loop: cuts the queued unreliable bunches to kMaxQueuedUnreliable, the oldest first.
    void drop_unreliable_backlog();
    /// Room for `count` more reliable bunches on the channel.
    bool has_window(uint32_t channel, size_t count) const;
    /// Numbers a reliable bunch and queues it: header, then `bits` bits of
    /// `payload` (byte-aligned data, or null when bits is 0).
    OutPtr queue_bunch(protocol::conn::BunchHeader header, const uint8_t* payload, size_t bits, PartTag tag = {});
    void accept(Queued&& q, std::vector<InBunch>& out);
    void complete(InBunch&& b, std::vector<InBunch>& out);
    protocol::emit::BunchWriter start_packet() const;
    std::vector<uint8_t> finish_packet(protocol::emit::BunchWriter& w, std::vector<OutPtr> tracked,
                                       Clock::time_point now);

    ConnectionConfig cfg_;
    protocol::conn::PacketNotify notify_;
    Clock::time_point last_receive_;
    Clock::time_point last_send_;
    bool dirty_acks_ = false;
    bool playing_ = false;
    bool closed_by_client_ = false;
    bool failed_ = false;
    size_t early_total_ = 0;  // bunches in in_queue_, over all channels

    std::deque<OutPtr> outgoing_;
    std::unordered_map<uint16_t, std::vector<OutPtr>> in_flight_;  // packet seq -> reliable bunches and chain parts
    std::unordered_map<uint32_t, int> out_reliable_;               // channel -> last sequence sent
    std::unordered_map<uint32_t, size_t> unacked_;                 // channel -> reliable in flight
    std::unordered_set<uint32_t> open_channels_;                   // actor channels we opened, not closed
    std::unordered_map<uint32_t, OutPtr> closing_;                 // closed, the close not acked: index -> the close
    ChainId next_chain_ = 1;
    std::deque<AckEvent> ack_events_;
    size_t unreliable_dropped_ = 0;
    std::unordered_map<uint32_t, int> in_reliable_;                // channel -> last sequence delivered
    std::unordered_map<uint32_t, std::map<int, Queued>> in_queue_; // early reliable bunches
    std::unordered_map<uint32_t, Partial> partial_;                // reliable partial being built
};

}} // namespace aoc::worldd
