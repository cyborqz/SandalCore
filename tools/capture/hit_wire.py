# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""The wire form of the hit-confirmation struct, read by a decoder that is independent of the C++ codec.

`gen_hits.py` cuts the captured hit RPCs with this file and writes what it read into `capture_hits.h`; the test
(`test_repl_hits.cpp`) reads the same bits with libaocnet's codec and compares.  Two implementations of the layout that
agree on every captured bit are the evidence that neither misreads it.

The layouts are the client exe's own (AOCClient-Win64-Shipping.exe, read-only), in the order the code runs (bit stream LSB
first; SerializeBits(v, n) is n raw bits, Serialize(p, n) is n * 8 raw bits, UBOOL a bool as 32 bits where a value above 1 is
an archive error):

ClientHitConfirmationsReliable / ClientHitConfirmations (Hits: FSerializedClientHitConfirmations), the RPC parameter:

    1       send bit (a struct parameter is always sent)
    struct  NetSerialize 0x1459ac190 -> 0x1459af5c0:
      11      Hits.Num as SerializeBits (CeilLogTwo(1999 + 1)); saving clamps it to 1000
      each    FSerializedClientHitConfirmation 0x1459abf90 -> 0x1459af460: SerializedData, a TArray<uint8>:
        15      Num as SerializeBits (CeilLogTwo(19999 + 1)); clamped to 10000
        Num*8   the bytes

SerializedData, read by the client with an FBitReader over Num * 8 bits (0x1463e2f90 -> 0x1463e1750):

    FHitInst 0x1463e1870:
      64 x 4  InstGuid, RecordGuid, SourceRecordType, SourceRecordGuid
      32      bPvPForceFlag (UBOOL)
      128 x 2 Instigator, Source: FIntrepidNetGUID, four uint32 (0x14141e960)
      7       Targets.Num as SerializeBits (CeilLogTwo(79 + 1), 0x1463d6180); clamped to 40
      each    FHitTargetInst 0x1463e1da0:
        128     HitComp: an FIntrepidNetGUID
        192     ImpactLocation: an FVector, three doubles (0x14141ed80)
        192     ImpactNormal: an FVector
        5       StatMods.Num as SerializeBits (CeilLogTwo(19 + 1), 0x1463d6070); clamped to 10
        each    StatModInst: Guid 64, Value float 32, EffectiveValue float 32
        32      Interrupts.Num (the archive's TArray operator, 0x14171fc20), each AbilityGuid 64
        32      bAttemptedInterrupt (UBOOL)
        8       Flags
        5       HitModInsts.Num as SerializeBits, clamped to 10
        each    HitModInst 0x1463e1c40: HitModGuid 64, Owner an FIntrepidNetGUID (128)
    32 x 2    bPredictedOnAutonomous, bPredictedOnSimulated (UBOOLs)

The blob ends in its last byte and its padding bits are zero.  The member names are the exe's reflection records'
(tools/ghidra/usmapgen.py).  Every reader refuses what the exe's writer never produces and raises WireError.

RequestHitVerification_Server (HitRequest: FAbilityHitRequest), the RPC parameter FRepLayout::SendPropertiesForRPC writes:

    1       send bit
    16      HitComponents.Num (uint16), each HitComponent an object (128 bits: ObjectId low, high, ServerId, Randomizer; an
            ObjectId of 1 names the object in place: an export flags byte (1 has a path, 2 no load, 4 has a checksum), with
            the path flag the outer's GUID (128 bits, never itself in place) and the object's name as an FString, with the
            checksum flag 32 bits) and ServerTimestamp float 32
    64 x 3  AbilityGuid, TargetingGuid, HitGuid
    quant   ActorLocation, an FVector_NetQuantize (UE::Net::WriteQuantizedVector: a 6-bit component width, ...)
    32      ActorYawAndPitch
"""
import struct
# == C-O-A | C | Y | B ==

from items_wire import BitWriter, Bits, WireError  # noqa: F401

MAX_HITS = 1000
MAX_BLOB = 10000
MAX_TARGETS = 40
MAX_STATS = 10
MAX_HITMODS = 10
MAX_INTERRUPTS = 64


def f64(r):
    return struct.unpack('<d', r.u64().to_bytes(8, 'little'))[0]


def read_target(r):
    t = {'comp': r.guid(), 'loc': (r.u64(), r.u64(), r.u64()), 'nrm': (r.u64(), r.u64(), r.u64())}
    n = r.bits(5)
    if n > MAX_STATS:
        raise WireError('StatMods %d' % n)
    t['mods'] = [(r.u64(), r.u32(), r.u32()) for _ in range(n)]
    n = r.u32()
    if n > MAX_INTERRUPTS:
        raise WireError('Interrupts %d' % n)
    t['interrupts'] = [r.u64() for _ in range(n)]
    t['attempted'] = r.ubool()
    t['flags'] = r.u8()
    n = r.bits(5)
    if n > MAX_HITMODS:
        raise WireError('HitModInsts %d' % n)
    t['hitmods'] = [(r.u64(), r.guid()) for _ in range(n)]
    return t


def read_hit(data):
    """One SerializedData blob (bytes) -> dict; raises WireError."""
    r = Bits(data, len(data) * 8)
    h = {'inst': r.u64(), 'rec': r.u64(), 'srt': r.u64(), 'srg': r.u64(), 'pvp': r.ubool(), 'instigator': r.guid(),
         'source': r.guid()}
    n = r.bits(7)
    if n > MAX_TARGETS:
        raise WireError('Targets %d' % n)
    h['targets'] = [read_target(r) for _ in range(n)]
    h['autonomous'] = r.ubool()
    h['simulated'] = r.ubool()
    left = len(data) * 8 - r.p
    if left >= 8 or (left and r.bits(left)):
        raise WireError('blob padding: %d bits left' % left)
    return h


def read_confirmations_params(r):
    """The whole RPC parameter -> list of hit dicts; raises WireError.  `r` must end exactly at the parameter's last bit."""
    if not r.bit():
        hits = []
    else:
        n = r.bits(11)
        if n > MAX_HITS:
            raise WireError('Hits %d' % n)
        hits = []
        for _ in range(n):
            size = r.bits(15)
            if size > MAX_BLOB:
                raise WireError('blob %d' % size)
            hits.append(read_hit(bytes(r.u8() for _ in range(size))))
    return hits

# C-O-A + C | Y | B

def read_quantized(r):
    """FVector_NetQuantize (UE::Net::ReadQuantizedVector): SerializeInt(info, 128) is seven bits, bits = info & 63 the width
    of each of the three components, info >> 6 the raw form.  -> ('scaled', x, y, z) with sign-extended components, or
    ('raw', 96 or 192 bits as an integer) when the width is 0."""
    info = r.bits(7)
    width, extra = info & 63, info >> 6
    if width:
        out = []
        for _ in range(3):
            v = r.bits(width)
            sign = 1 << (width - 1)
            out.append((v ^ sign) - sign)
        return ('scaled',) + tuple(out)
    return ('raw', r.bits(192 if extra else 96))


def read_request_params(r):
    """RequestHitVerification_Server's parameter -> dict (None when the send bit is 0); raises WireError.  `r` must end
    exactly at the parameter's last bit."""
    if not r.bit():
        return None
    n = r.bits(16)
    comps = []
    for _ in range(n):
        lo, hi, server, rand = r.u32(), r.u32(), r.u32(), r.u32()
        if lo | (hi << 32) == 1:      # named in place
            flags = r.u8()
            if flags > 7:
                raise WireError('export flags %d' % flags)
            outer, path, checksum = None, None, 0
            if flags & 1:
                outer = r.guid()
                if outer[0] | (outer[1] << 32) == 1:
                    raise WireError('inline outer')
                path = r.fstring()
                if not path:
                    raise WireError('empty path')
            if flags & 4:
                checksum = r.u32()
            comps.append(('inline', (hi << 32) | lo, server, rand, flags, outer, path, checksum, r.u32()))
        else:
            comps.append(('obj', (hi << 32) | lo, server, rand, r.u32()))
    req = {'components': comps, 'ability': r.u64(), 'targeting': r.u64(), 'hit': r.u64()}
    req['location'] = read_quantized(r)
    req['yaw_pitch'] = r.u32()
    return req


# ---- the writer, for the synthetic hits ---------------------------------------------------------------------------------

def write_hit(w, h):
    w.w(h['inst'], 64), w.w(h['rec'], 64), w.w(h['srt'], 64), w.w(h['srg'], 64)
    w.w(h['pvp'], 32)
    w.guid(h['instigator']), w.guid(h['source'])
    w.w(len(h['targets']), 7)
    for t in h['targets']:
        w.guid(t['comp'])
        for v in t['loc'] + t['nrm']:
            w.w(v, 64)
        w.w(len(t['mods']), 5)
        for g, a, b in t['mods']:
            w.w(g, 64), w.w(a, 32), w.w(b, 32)
        w.w(len(t['interrupts']), 32)
        for a in t['interrupts']:
            w.w(a, 64)
        w.w(t['attempted'], 32)
        w.w(t['flags'], 8)
        w.w(len(t['hitmods']), 5)
        for g, o in t['hitmods']:
            w.w(g, 64), w.guid(o)
    w.w(h['autonomous'], 32)
    w.w(h['simulated'], 32)


def write_confirmations_params(hits):
    """-> (bytes, bits): the RPC parameter for `hits` (dicts as read_hit makes them)."""
    w = BitWriter()
    w.w(1, 1)
    w.w(len(hits), 11)
    for h in hits:
        inner = BitWriter()
        write_hit(inner, h)
        data, _ = inner.finish()
        w.w(len(data), 15)
        for b in data:
            w.w(b, 8)
    return w.finish()
# ~ Cyb :: C-O-A ~


# ---- canonical text dumps: what the generator writes into the header and the test rebuilds from its own reading -------------

def s64(v):
    return v - (1 << 64) if v >= 1 << 63 else v


def g(guid):
    return ''.join('%08x' % x for x in guid)


def dump_target(t):
    return 'comp=%s loc=%s nrm=%s mods=[%s] int=[%s] att=%d fl=%d hm=[%s]' % (
        g(t['comp']), ','.join('%016x' % v for v in t['loc']), ','.join('%016x' % v for v in t['nrm']),
        ','.join('%d:%08x:%08x' % (s64(a), b, c) for a, b, c in t['mods']), ','.join('%d' % s64(a) for a in t['interrupts']),
        t['attempted'], t['flags'], ','.join('%d:%s' % (s64(a), g(o)) for a, o in t['hitmods']))


def dump_hit(h):
    return 'inst=%d rec=%d srt=%d srg=%d pvp=%d instig=%s src=%s pa=%d ps=%d targets=[%s]' % (
        s64(h['inst']), s64(h['rec']), s64(h['srt']), s64(h['srg']), h['pvp'], g(h['instigator']), g(h['source']),
        h['autonomous'], h['simulated'], ';'.join(dump_target(t) for t in h['targets']))


def dump_hits(hits):
    return 'hits=%d {%s}' % (len(hits), ' | '.join(dump_hit(h) for h in hits))


def dump_component(c):
    if c[0] == 'obj':
        return 'o:%d.%d.%d:%08x' % c[1:]
    return 'i:%d.%d.%d:fl=%d:outer=%s:path=%s:ck=%08x:%08x' % (
        c[1], c[2], c[3], c[4], g(c[5]) if c[5] else '-', c[6].encode('utf-8').hex() if c[6] is not None else '-', c[7], c[8])


def dump_request(q):
    if q is None:
        return 'none'
    loc = ('%d,%d,%d' % q['location'][1:]) if q['location'][0] == 'scaled' else 'raw'
    return 'comps=[%s] ability=%d targeting=%d hit=%d loc=%s yp=%08x' % (
        ','.join(dump_component(c) for c in q['components']), s64(q['ability']), s64(q['targeting']), s64(q['hit']), loc,
        q['yaw_pitch'])
