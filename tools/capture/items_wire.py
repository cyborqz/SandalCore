# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""The wire forms of AoC's item structs, read by a decoder that is independent of the C++ codecs.

`gen_items.py` cuts the captured item RPCs with this file and writes what it read into `capture_items.h`; the test
(`test_repl_items.cpp`) reads the same bits with libaocnet's codecs and compares.  Two implementations of the layout
that agree on every captured bit are the evidence that neither misreads it.

The layouts are the client exe's own (AOCClient-Win64-Shipping.exe, read-only), in the order the code runs:

FItemInstance::NetSerialize  0x146540110 (bit stream LSB first; `Serialize(p, n)` is n * 8 raw bits, UBOOL a bool as 32 bits
(a value above 1 is an archive error), packed = SerializeIntPacked: 7 bits a byte, bit 0 "more follows"):

    1       bHasItem            0: nothing else follows (the empty item)
    64      ItemRecordId
    packed  Quantity, Quality, EnchantmentLevel, TemperingValue (four int32)
    gems    0x1465187d0: int32 count (the exe refuses a negative count and, on a net archive, one above 0x80000), each
            element (0x146544130) SocketIndex 8, ItemRecordId 64, SocketRecordId 64, Quality 32, a UBOOL 32
    32      Durability (float)
    32      bIsStolen as a UBOOL
    128+64  GUID (four int32), CreationTime (an FDateTime: ticks)
    128     CrateGuid
    1       bIsStolen again, as one bit
    128+64  GUID and CreationTime again (the exe's code serializes them twice into the same members)
    64      NodeGuidPurchasedFrom
    128     CrateGuid again
    64      LockExpiration
    stats   0x1465181c0: a 6-bit count (the exe refuses more than 31), each StatId 64, StatModValue 32
    cooldowns 0x146518390: int32 count, each RecordGuid 64, DateTimeStart 64 (a double), SavedCharges 32 (element 0x14651c8c0)
    custom  0x146518570: int32 count, each a byte and an FString
    64      SynchronizedTimestamp (double)

StorageEventContainer 0x1464614d0: the object reference (128 bits), a TArray of {Slot int32, Count int32, FGuid} (int32 count),
the StorageEventStatus byte.

FItemStorageEvent::NetSerialize 0x145c58f20: MoveSourceContainer, ItemData, a UBOOL (a further optional struct follows when it is
1: not decoded), EventType byte, InstigatingClient FGuid, three TOptionals (UBOOL, then FGuid / int64 / FGuid), TargetContainers
(int32 count and the containers).

ClientReceiveStoragePayload(TargetStorage, NewStorageData): see read_payload_params.

Every reader refuses what the exe's writer never produces (a second copy of a duplicated member that differs from the first, a
packed int that is not minimal, a count above the bounds below) and raises WireError.
"""
import struct

MAX_GEMS = 64
MAX_STATS = 31
MAX_COOLDOWNS = 64
MAX_CUSTOM = 64
MAX_EVENT_CONTAINERS = 64
MAX_EVENT_SLOTS = 4096
MAX_FSTRING = 65535


class WireError(Exception):
    pass


class Bits:
    def __init__(self, data, nbits, pos=0):
        self.d, self.n, self.p = data, nbits, pos
        self.item_spans = []      # (start, end) of every FItemInstance read, the empty ones too
# -- C:Y:B & C-O-A --

    def bits(self, k):
        if self.p + k > self.n:
            raise WireError('read past the end')
        v = 0
        for i in range(k):
            q = self.p + i
            v |= ((self.d[q >> 3] >> (q & 7)) & 1) << i
        self.p += k
        return v

    def bit(self):
        return self.bits(1)

    def u8(self):
        return self.bits(8)

    def u32(self):
        return self.bits(32)

    def u64(self):
        return self.bits(64)

    def ubool(self):
        v = self.bits(32)
        if v > 1:
            raise WireError('UBOOL %d' % v)
        return v

    def f32(self):
        return struct.unpack('<f', self.u32().to_bytes(4, 'little'))[0]

    def f64(self):
        return struct.unpack('<d', self.u64().to_bytes(8, 'little'))[0]

    def guid(self):
        return tuple(self.u32() for _ in range(4))

    def packed32(self):
        v = 0
        for i in range(5):
            b = self.u8()
            payload = b >> 1
            v |= payload << (7 * i)
            if not b & 1:
                if i and not payload:
                    raise WireError('packed int not minimal')
                if v > 0xFFFFFFFF:
                    raise WireError('packed int over 32 bits')
                return v
        raise WireError('packed int over 5 bytes')

    def count32(self, maximum):
        n = self.u32()
        if n > maximum:
            raise WireError('count %d over %d' % (n, maximum))
        return n

    def fstring(self):
        n = self.u32()
        if n >= 1 << 31:
            n -= 1 << 32
        if n == 0:
            return ''
        length = abs(n)
        if length > MAX_FSTRING + 1:
            raise WireError('FString length %d' % length)
        if n > 0:
            raw = bytes(self.u8() for _ in range(length))
            if raw[-1] != 0 or 0 in raw[:-1] or any(c >= 0x80 for c in raw):
                raise WireError('FString bytes')
            return raw[:-1].decode('ascii')
        units = [self.bits(16) for _ in range(length)]
        if units[-1] != 0 or 0 in units[:-1]:
            raise WireError('FString units')
        return b''.join(u.to_bytes(2, 'little') for u in units[:-1]).decode('utf-16-le')


def read_item(r):
    """An FItemInstance as a dict; None for the empty item (one 0 bit).  r.item_spans gets where it sat."""
    start = r.p
    if not r.bit():
        r.item_spans.append((start, r.p))
        return None
    d = {}
    d['record_id'] = r.u64()
    for k in ('quantity', 'quality', 'enchantment_level', 'tempering_value'):
        d[k] = r.packed32()
    d['gems'] = [dict(socket_index=r.u8(), item_record_id=r.u64(), socket_record_id=r.u64(), quality=r.u32(), flag=r.ubool())
                 for _ in range(r.count32(MAX_GEMS))]
    d['durability'] = r.u32()                      # float bits
    stolen32 = r.ubool()
    d['guid'] = r.guid()
    d['creation_time'] = r.u64()
    d['crate_guid'] = r.guid()
    d['stolen'] = r.bit()
    if d['stolen'] != stolen32:
        raise WireError('bIsStolen differs between its two copies')
    if (r.guid(), r.u64()) != (d['guid'], d['creation_time']):
        raise WireError('GUID / CreationTime differ between their two copies')
    d['node_guid_purchased_from'] = r.u64()
    if r.guid() != d['crate_guid']:
        raise WireError('CrateGuid differs between its two copies')
    d['lock_expiration'] = r.u64()
    n = r.bits(6)
    if n > MAX_STATS:
        raise WireError('persisted stats %d' % n)
    d['stats'] = [dict(stat_id=r.u64(), value=r.u32()) for _ in range(n)]
    d['cooldowns'] = [dict(record_guid=r.u64(), date_time_start=r.u64(), saved_charges=r.u32())
                      for _ in range(r.count32(MAX_COOLDOWNS))]
    d['custom'] = [dict(type=r.u8(), text=r.fstring()) for _ in range(r.count32(MAX_CUSTOM))]
    d['synchronized_timestamp'] = r.u64()          # double bits
    r.item_spans.append((start, r.p))
    return d


def read_container(r):
    c = {'object': (r.u64(), r.u32(), r.u32())}
    if c['object'][0] == 1:
        raise WireError('object with ObjectId 1')
    c['slots'] = [dict(slot=r.u32(), count=r.u32(), item_guid=r.guid()) for _ in range(r.count32(MAX_EVENT_SLOTS))]
    c['status'] = r.u8()
    return c


def read_event(r):
    e = {'move_source': read_container(r), 'item': read_item(r)}
    if r.ubool():
        raise WireError('FItemStorageEvent optional struct set (not decoded)')
    e['type'] = r.u8()
    e['instigator'] = r.guid()
    e['extra_guid_a'] = r.guid() if r.ubool() else None
    e['extra_i64'] = r.u64() if r.ubool() else None
    e['extra_guid_b'] = r.guid() if r.ubool() else None
    e['targets'] = [read_container(r) for _ in range(r.count32(MAX_EVENT_CONTAINERS))]
    return e


def read_event_params(r):
    """The parameters of ClientReceiveStorageEvent(ItemEvent) / ServerRequestStorageEvent(StorageEvent): a send bit and the event."""
    if not r.bit():
        return None
    return read_event(r)


def read_payload_params(r):
    """ClientReceiveStoragePayload(TargetStorage, NewStorageData): the parameters as FRepLayout::SendPropertiesForRPC writes them:
    a send bit and the command values in order, an array as a uint16 count and each element's commands."""
    p = {'target': None, 'data': None}
    if r.bit():
        p['target'] = (r.u64(), r.u32(), r.u32())
        if p['target'][0] == 1:
            raise WireError('TargetStorage with ObjectId 1')
    if r.bit():
        d = {}
        d['container_name'] = r.fstring()
        d['maximum_weight'] = r.u32()
        d['maximum_items'] = r.u32()
        d['loot_fountain_on_death'] = r.bit()
        d['item_drop_on_destruction_percent'] = r.u32()
        d['maximum_items_to_drop_percent'] = r.u32()
        d['minimum_impulse'] = (r.u64(), r.u64(), r.u64())
        d['maximum_impulse'] = (r.u64(), r.u64(), r.u64())
        d['randomize_angular'] = r.bit()
        slots = []
        for _ in range(r.bits(16)):
            s = {'item': read_item(r), 'slot': r.u32(), 'transaction_lock': r.guid(), 'check_loot_permission': r.bit()}
            s['loot_permission'] = [r.guid() for _ in range(r.bits(16))]
            slots.append(s)
        d['storage'] = slots
        d['client_retrieve_only'] = r.bit()
        d['dimension'] = (r.u64(), r.u64())
        d['override_matrix'] = [[r.u32() for _ in range(r.bits(16))] for _ in range(r.bits(16))]
        p['data'] = d
    return p
# C-Y-B + C | O | A


def read_trigger_params(r):
    """ServerTriggerAbility's parameters as the retail client sends them, for the calls whose Targets
    name their objects by GUID: AbilityGuid, ClientInstanceId, AnimationIndex, InputVector, bPvpForceFlag, Targets, the number of
    TargetTransforms (nine doubles each), ItemData."""
    t = {}
    t['ability_guid'] = r.u64() if r.bit() else 0
    t['client_instance_id'] = r.u32() if r.bit() else 0
    t['animation_index'] = r.u32() if r.bit() else 0
    t['input_vector'] = (r.u64(), r.u64()) if r.bit() else (0, 0)
    t['pvp_force'] = r.bit()
    t['targets'] = []
    if r.bit():
        for _ in range(r.bits(16)):
            o = (r.u64(), r.u32(), r.u32())
            if o[0] == 1:
                raise WireError('a Target named in place (not decoded here)')
            t['targets'].append(o)
    t['transforms'] = 0
    if r.bit():
        t['transforms'] = r.bits(16)
        for _ in range(t['transforms'] * 9):
            r.u64()
    if not r.bit():
        raise WireError('ItemData left at the default')
    t['slot'] = r.u32()
    t['item'] = read_item(r)
    t['location'] = r.bits(5)
    return t


class BitWriter:
    """The inverse of Bits, for the synthetic instances (the variants no capture shows)."""

    def __init__(self):
        self.b = []

    def w(self, v, n):
        self.b.extend((v >> i) & 1 for i in range(n))

    def packed32(self, v):
        while True:
            payload = v & 0x7f
            v >>= 7
            self.w((payload << 1) | (1 if v else 0), 8)
            if not v:
                return

    def guid(self, g):
        for x in g:
            self.w(x, 32)

    def fstring(self, text):
        if not text:
            self.w(0, 32)
        elif all(ord(c) < 0x80 for c in text):
            self.w(len(text) + 1, 32)
            for c in text:
                self.w(ord(c), 8)
            self.w(0, 8)
        else:
            units = [int.from_bytes(text.encode('utf-16-le')[i:i + 2], 'little') for i in range(0, len(text.encode('utf-16-le')), 2)]
            self.w((1 << 32) - (len(units) + 1), 32)
            for u in units:
                self.w(u, 16)
            self.w(0, 16)

    def finish(self):
        n = len(self.b)
        out = bytearray((n + 7) // 8)
        for i, bit in enumerate(self.b):
            if bit:
                out[i >> 3] |= 1 << (i & 7)
        return bytes(out), n


def write_item(w, d):
    """An instance dict (read_item's) as the exe writes it; None is the empty item."""
    if d is None:
        w.w(0, 1)
        return
    w.w(1, 1)
    w.w(d['record_id'], 64)
    for k in ('quantity', 'quality', 'enchantment_level', 'tempering_value'):
        w.packed32(d[k])
    w.w(len(d['gems']), 32)
    for m in d['gems']:
        w.w(m['socket_index'], 8)
        w.w(m['item_record_id'], 64)
        w.w(m['socket_record_id'], 64)
        w.w(m['quality'], 32)
        w.w(m['flag'], 32)
    w.w(d['durability'], 32)
    w.w(d['stolen'], 32)
    w.guid(d['guid'])
    w.w(d['creation_time'], 64)
    w.guid(d['crate_guid'])
    w.w(d['stolen'], 1)
    w.guid(d['guid'])
    w.w(d['creation_time'], 64)
    w.w(d['node_guid_purchased_from'], 64)
    w.guid(d['crate_guid'])
    w.w(d['lock_expiration'], 64)
    w.w(len(d['stats']), 6)
    for s in d['stats']:
        w.w(s['stat_id'], 64)
        w.w(s['value'], 32)
    w.w(len(d['cooldowns']), 32)
    for c in d['cooldowns']:
        w.w(c['record_guid'], 64)
        w.w(c['date_time_start'], 64)
        w.w(c['saved_charges'], 32)
    w.w(len(d['custom']), 32)
    for c in d['custom']:
        w.w(c['type'], 8)
        w.fstring(c['text'])
    w.w(d['synchronized_timestamp'], 64)


def item_kind(d):
    """A label for the field variants an instance shows (None for the empty item)."""
    if d is None:
        return 'empty'
    tags = []
    if d['gems']:
        tags.append('gems')
    if d['stats']:
        tags.append('stats')
    if d['cooldowns']:
        tags.append('cooldowns')
    if d['custom']:
        tags.append('custom')
    if d['stolen']:
        tags.append('stolen')
    if d['crate_guid'] != (0, 0, 0, 0):
        tags.append('crate')
    if d['enchantment_level']:
        tags.append('enchanted')
    if d['tempering_value']:
        tags.append('tempered')
    return '+'.join(tags) or 'plain'
# [C-O-A ~ Cyb]


def cut(data, start, n):
    """Bits [start, start + n) of `data` re-packed from bit 0, as hex."""
    out = bytearray((n + 7) // 8)
    for i in range(n):
        q = start + i
        if (data[q >> 3] >> (q & 7)) & 1:
            out[i >> 3] |= 1 << (i & 7)
    return out.hex()


# ---- canonical text dumps: what the generator writes into the header and the test rebuilds from its own reading ------------

def g(guid):
    return ''.join('%08x' % x for x in guid)


def obj(o):
    return '%d.%d.%d' % o


def dump_item(d):
    if d is None:
        return 'empty'
    gems = ','.join('%d:%d:%d:%d:%d' % (m['socket_index'], m['item_record_id'], m['socket_record_id'], m['quality'], m['flag'])
                    for m in d['gems'])
    stats = ','.join('%d:%08x' % (s['stat_id'], s['value']) for s in d['stats'])
    cds = ','.join('%d:%016x:%d' % (c['record_guid'], c['date_time_start'], c['saved_charges']) for c in d['cooldowns'])
    custom = ','.join('%d:%s' % (c['type'], c['text'].encode('utf-8').hex()) for c in d['custom'])   # the UTF-8 bytes in hex
    ct = d['creation_time'] - (1 << 64 if d['creation_time'] >= 1 << 63 else 0)
    lock = d['lock_expiration'] - (1 << 64 if d['lock_expiration'] >= 1 << 63 else 0)
    return ('rec=%d qty=%d qual=%d ench=%d temp=%d gems=[%s] dur=%08x stolen=%d guid=%s ct=%d crate=%s node=%d lock=%d '
            'stats=[%s] cd=[%s] custom=[%s] ts=%016x' % (
                d['record_id'], d['quantity'], d['quality'], d['enchantment_level'], d['tempering_value'], gems,
                d['durability'], d['stolen'], g(d['guid']), ct, g(d['crate_guid']), d['node_guid_purchased_from'], lock,
                stats, cds, custom, d['synchronized_timestamp']))


def dump_container(c):
    return '%s/%s/st=%d' % (obj(c['object']), ','.join('%d:%d:%s' % (s['slot'] - (1 << 32 if s['slot'] >= 1 << 31 else 0),
                                                                   s['count'] - (1 << 32 if s['count'] >= 1 << 31 else 0),
                                                                   g(s['item_guid'])) for s in c['slots']), c['status'])


def short_item(d):
    return 'empty' if d is None else '%d@%s' % (d['record_id'], g(d['guid']))


def dump_event(e):
    return 'type=%d src=%s item=%s instig=%s xa=%s xi=%s xb=%s targets=[%s]' % (
        e['type'], dump_container(e['move_source']), short_item(e['item']), g(e['instigator']),
        g(e['extra_guid_a']) if e['extra_guid_a'] else '-', e['extra_i64'] if e['extra_i64'] is not None else '-',
        g(e['extra_guid_b']) if e['extra_guid_b'] else '-', ';'.join(dump_container(c) for c in e['targets']))


def dump_payload(p):
    d = p['data']
    if d is None:
        return 'target=%s nodata' % (obj(p['target']) if p['target'] else '-')
    filled = [(i, s) for i, s in enumerate(d['storage']) if s['item'] is not None]
    return ('target=%s name=%s weight=%08x max=%d fountain=%d drop=%08x dropmax=%08x imin=%s imax=%s angular=%d slots=%d '
            'filled=%d retrieve=%d dim=%s matrix=%s [%s]' % (
                obj(p['target']) if p['target'] else '-', d['container_name'], d['maximum_weight'], d['maximum_items'] - (
                    1 << 32 if d['maximum_items'] >= 1 << 31 else 0), d['loot_fountain_on_death'],
                d['item_drop_on_destruction_percent'], d['maximum_items_to_drop_percent'],
                ','.join('%016x' % x for x in d['minimum_impulse']), ','.join('%016x' % x for x in d['maximum_impulse']),
                d['randomize_angular'], len(d['storage']), len(filled), d['client_retrieve_only'],
                ','.join('%016x' % x for x in d['dimension']),
                ';'.join(','.join('%d' % (c - (1 << 32 if c >= 1 << 31 else 0)) for c in col) for col in d['override_matrix']),
                ','.join('%d:%d:%s' % (i, s['slot'] - (1 << 32 if s['slot'] >= 1 << 31 else 0), short_item(s['item']))
                         for i, s in filled)))
