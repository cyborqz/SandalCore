# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Decode actor-channel opens the way the AoC client reads them.

Input is JSON lines of raw bunches, as printed by replay_bunches (the original
server's traffic) or the native-bunch harness (ours).  For each actor channel
the decoder follows the client:

  per raw bunch   UChannel::ReceivedRawBunch -> ReceiveNetGUIDBunch when the
                  bunch has package-map exports:
                    bHasRepLayoutExport(1)=0, int32 count, objects (exporting)
  partial chain   merged by ReceivedNextBunch; then, for the merged bunch,
                  must-be-mapped GUIDs: uint16 count + 128-bit GUIDs
  open            UPackageMapClient::SerializeNewActor: actor object; if the
                  GUID is dynamic (ObjectId even): archetype, level,
                  location / rotation / scale / velocity; then, for a
                  PlayerController, APlayerController::OnActorChannelOpen:
                  NetPlayerIndex(8) + ClientHandshakeId(32) (AoC's net version
                  is below CloseChildConnection, so no connection identifier)
  content blocks  until the end: bHasRepLayout, bIsActor, [subobject header],
                  SIP payload bits, payload (skipped)

Object reference (InternalLoadObject, AoC 1442740f0): FGuid-style 128 bits
(ObjectId lo/hi, ServerId, Randomizer); ObjectId 0 is null; 8-bit export
flags when ObjectId is 1 or inside an export; flags&1 (bHasPath) -> outer
object + FString name; flags&4 -> uint32 checksum.

Variants the client chooses by engine network version or cvar are switches:
--vector new|legacy (quantized vector format), --rotation raw|compressed.

    replay_bunches fixtures/replay_data.bin --to 140 | python tools/decode_actor_open.py -
    python tools/decode_actor_open.py bunches.jsonl --channel 3 --verbose
"""
import argparse
import json
import sys


class DecodeError(Exception):
    pass


class BitReader:
    def __init__(self, data: bytes, nbits: int):
        self.data, self.end, self.pos = data, nbits, 0

    def remaining(self):
        return self.end - self.pos

    def read(self, n):
        if self.pos + n > self.end:
            raise DecodeError(f'read of {n} bits at {self.pos} overruns {self.end}')
        v = 0
        for i in range(n):
            p = self.pos + i
            v |= ((self.data[p >> 3] >> (p & 7)) & 1) << i
        self.pos += n
        return v

    def bit(self):
        return self.read(1)
# built with C-Y-B and C | O | A

    def sip(self):          # SerializeIntPacked
        v, shift = 0, 0
        while True:
            b = self.read(8)
            v |= (b >> 1) << shift
            shift += 7
            if not b & 1:
                return v
            if shift > 63:
                raise DecodeError('SerializeIntPacked too long')

    def sint(self, max_value):   # SerializeInt(v, max)
        v, mask = 0, 1
        while v + mask < max_value and mask:
            if self.bit():
                v |= mask
            mask <<= 1
        return v

    def int32(self):
        v = self.read(32)
        return v - (1 << 32) if v & 0x80000000 else v

    def fstring(self):
        n = self.int32()
        if n == 0:
            return ''
        if abs(n) > 4096:
            raise DecodeError(f'implausible FString length {n}')
        if n > 0:
            s = bytes(self.read(8) for _ in range(n))
            return s[:-1].decode('latin-1')
        s = ''.join(chr(self.read(16)) for _ in range(-n))
        return s[:-1]


def read_object(r, exporting, depth=0):
    if depth > 16:
        raise DecodeError('object outer chain too deep')
    lo, hi, srv, rnd = r.read(32), r.read(32), r.read(32), r.read(32)
    obj = {'id': (hi << 32) | lo, 'srv': srv, 'rnd': rnd}
    if obj['id'] == 0:
        return obj
    if obj['id'] == 1 or exporting:
        flags = r.read(8)
        obj['flags'] = flags
        if flags & 1:
            obj['outer'] = read_object(r, exporting, depth + 1)
            obj['name'] = r.fstring()
            if flags & 4:
                obj['checksum'] = r.read(32)
    return obj


def path_of(obj):
    parts = []
    while obj and 'name' in obj:
        parts.append(obj['name'])
        obj = obj.get('outer')
    return '.'.join(reversed(parts)) if parts else f"guid {obj['id'] if obj else '?'}"


def read_exports(r):
    if r.bit():
        raise DecodeError('bHasRepLayoutExport=1 (field-export compat block) not handled')
    count = r.int32()
    if not 0 <= count <= 2048:
        raise DecodeError(f'implausible export count {count}')
    return [read_object(r, True) for _ in range(count)]


def read_vector(r, variant):
    if variant == 'new':          # UE::Net::ReadQuantizedVector
        info = r.sint(128)
        bits, extra = info & 63, info >> 6
        if bits:
            comps = []
            for _ in range(3):
                v = r.read(bits)
                sign = 1 << (bits - 1)
                comps.append((v ^ sign) - sign)
            return {'q': bits, 'scaled': extra, 'v': comps}
        r.read(192 if extra else 96)
        return {'raw': 'double' if extra else 'float'}
    bits = r.sint(25)             # legacy SerializePackedVector<10, 24>
    comps = [r.read(bits) - (1 << (bits - 1)) for _ in range(3)] if bits else [0, 0, 0]
    return {'q': bits, 'v': comps}


def cond_vector(r, variant):
    if not r.bit():
        return None
    if r.bit():                   # bShouldQuantize (net version > 12)
        return read_vector(r, variant)
    r.read(192)                   # FArchive << FVector3d
    return {'raw': 'double'}

# C O A : Cyb

def read_new_actor(r, args, paths):
    actor = read_object(r, False)
    out = {'actor': actor}
    if actor['id'] and not actor['id'] & 1:        # dynamic
        out['archetype'] = read_object(r, False)
        out['level'] = read_object(r, False)
        out['location'] = cond_vector(r, args.vector)
        if r.bit():
            if args.rotation == 'compressed':
                out['rotation'] = [r.read(16) if r.bit() else 0 for _ in range(3)]
            else:
                r.read(192)
                out['rotation'] = 'raw'
        out['scale'] = cond_vector(r, args.vector)
        out['velocity'] = cond_vector(r, args.vector)
        out['archetype_path'] = paths.get(out['archetype']['id'], '')
        if 'PlayerController' in out['archetype_path']:
            out['net_player_index'] = r.read(8)
            out['client_handshake_id'] = r.read(32)
    return out


def read_content_blocks(r):
    blocks = []
    while r.remaining() > 0:
        start = r.pos
        rep, is_actor = r.bit(), r.bit()
        blk = {'at': start, 'rep': rep, 'actor': is_actor}
        if not is_actor:
            blk['sub'] = read_object(r, False)['id']
            if r.bit():
                blk['stably_named'] = 1
            else:
                if r.bit():
                    blk['destroy'] = r.read(8)
                else:
                    blk['class'] = read_object(r, False)['id']
                if not r.bit():
                    blk['outer'] = read_object(r, False)['id']
        payload = r.sip()
        if payload > r.remaining():
            raise DecodeError(f'content block at {start}: payload {payload} bits overruns '
                              f'{r.remaining()} left')
        r.pos += payload
        blk['payload'] = payload
        blocks.append(blk)
    return blocks


def chains(bunches):
    """Yield (first raw bunch, exports per raw bunch, merged payload bytes, bits)."""
    pending = None
    for b in bunches:
        data = bytes.fromhex(b['hex'])
        r = BitReader(data, b['bits'])
        exports = read_exports(r) if b['exports'] else []
        rest = [((data[p >> 3] >> (p & 7)) & 1) for p in range(r.pos, b['bits'])]
        if b['partial']:
            if b['pinit']:
                pending = {'first': b, 'exports': [exports], 'bits': rest}
            elif pending is None:
                continue
            else:
                pending['exports'].append(exports)
                pending['bits'] += rest
            if not b['pfinal']:
                continue
            item, pending = pending, None
        else:
            item = {'first': b, 'exports': [exports], 'bits': rest}
        bits = item['bits']
        packed = bytearray((len(bits) + 7) // 8)
        for i, v in enumerate(bits):
            packed[i >> 3] |= v << (i & 7)
        yield item['first'], item['exports'], bytes(packed), len(bits)


def decode_channel(bunches, args, paths):
    results = []
    for first, exports, data, nbits in chains(bunches):
        for group in exports:
            for o in group:
                paths[o['id']] = path_of(o)
        res = {'pkt': first['pkt'], 'ch': first['ch'], 'chseq': first['chseq'],
               'open': first['open'], 'bits': nbits,
               'exports': [path_of(o) for group in exports for o in group]}
        r = BitReader(data, nbits)
        try:
            if first['mustmap']:
                n = r.read(16)
                res['mustmap'] = [read_object(r, False)['id'] for _ in range(n)]
            if first['open']:
                res['new_actor'] = read_new_actor(r, args, paths)
            res['blocks'] = read_content_blocks(r)
            res['ok'] = True
        except DecodeError as e:
            res['ok'] = False
            res['error'] = f'{e} (at bit {r.pos} of {nbits})'
        results.append(res)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('input', help="JSON lines of raw bunches, or '-' for stdin")
    ap.add_argument('--channel', type=int, help='only this channel')
    ap.add_argument('--vector', choices=('new', 'legacy'), default='new')
    ap.add_argument('--rotation', choices=('raw', 'compressed'), default='compressed')
    ap.add_argument('--opens-only', action='store_true', help='only chains that open a channel')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    src = sys.stdin if args.input == '-' else open(args.input, encoding='utf-8')
    by_channel = {}
    for line in src:
        b = json.loads(line)
        if b['ctrl'] and not b['open']:
            continue
        if args.channel is not None and b['ch'] != args.channel:
            continue
        by_channel.setdefault(b['ch'], []).append(b)
# -- C-Y-B . CoA --

    total = ok = 0
    paths = {}   # NetGUID -> exported path, shared across channels like the client's GUID cache
    for ch in sorted(by_channel, key=lambda c: by_channel[c][0]['pkt']):
        for res in decode_channel(by_channel[ch], args, paths):
            if args.opens_only and not res['open']:
                continue
            total += 1
            ok += res['ok']
            if args.verbose or not res['ok']:
                head = (f"pkt {res['pkt']} ch {res['ch']} chSeq {res['chseq']} "
                        f"{'OPEN ' if res['open'] else ''}{res['bits']} bits: "
                        f"{'ok' if res['ok'] else 'FAIL ' + res['error']}")
                print(head)
                if args.verbose:
                    if res['exports']:
                        print('   exports:', '; '.join(res['exports']))
                    if 'new_actor' in res:
                        na = res['new_actor']
                        print('   actor:', na['actor']['id'], 'archetype:',
                              na.get('archetype_path') or na.get('archetype', {}).get('id'),
                              'level:', na.get('level', {}).get('id'),
                              'loc:', na.get('location'), 'rot:', na.get('rotation'),
                              'scale:', na.get('scale'), 'vel:', na.get('velocity'))
                    for blk in res.get('blocks', []):
                        print('   block', blk)
    print(f'{ok} of {total} chains decode to their last bit '
          f'(vector={args.vector}, rotation={args.rotation})')
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
