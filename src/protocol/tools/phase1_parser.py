#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""
Phase 1: Definitive UE5/AoC Packet Parser
==========================================
Parses capture JSONL files and extracts:
- All bunch headers with correct bit alignment
- NetGUID exports (recursive InternalLoadObject)
- RepLayout field exports
- SerializeNewActor data
- Content block headers + payload sizes

Key fix over v3: hasServerFrameTime is read REGARDLESS of bHasPktInfo.

Usage:
    python phase1_parse_capture.py [capture.jsonl] [--json output.json] [--summary]
"""
import json, sys, os, struct, argparse
from collections import defaultdict, Counter

# ═══════════════════════════════════════════════════════════════
# Bit-level primitives
# ═══════════════════════════════════════════════════════════════

def read_bit(data, pos):
    byte_idx = pos >> 3
    if byte_idx >= len(data):
        return 0
    return (data[byte_idx] >> (pos & 7)) & 1

def read_bits_le(data, off, n):
    """Read n bits starting at bit offset 'off', LSB first."""
    v = 0
    for i in range(n):
        bp = off + i
        byte_idx = bp >> 3
        if byte_idx < len(data):
            v |= ((data[byte_idx] >> (bp & 7)) & 1) << i
    return v, off + n

def read_uint8(data, pos):
    return read_bits_le(data, pos, 8)

def read_uint16(data, pos):
    return read_bits_le(data, pos, 16)

def read_uint32(data, pos):
    return read_bits_le(data, pos, 32)

def serialize_int(data, pos, max_val):
    """UE5 SerializeInt: variable-length encoding up to max_val."""
    v = 0
    mask = 1
    while (v + mask) < max_val and mask < (1 << 32):
        bit = read_bit(data, pos)
        if bit:
            v |= mask
        pos += 1
        mask <<= 1
    return v, pos

def serialize_int_packed(data, pos, end_pos=None):
    """UE5 SerializeIntPacked (uint32): bit0=continue, bits1-7=data.
    If end_pos is given, returns (None, pos) on overflow."""
    v = 0
    shift = 0
    for _ in range(5):
        if end_pos is not None and pos + 8 > end_pos:
            return None, pos
        byte_val, pos = read_bits_le(data, pos, 8)
        v |= ((byte_val >> 1) & 0x7F) << shift
        if not (byte_val & 1):
            break
        shift += 7
    return v, pos
# == C:Y:B | CoA ==

def serialize_int_packed64(data, pos, end_pos=None):
    """UE5 SerializeIntPacked64 (uint64): same format, up to 10 bytes.
    If end_pos is given, returns (None, pos) on overflow."""
    v = 0
    shift = 0
    for _ in range(10):
        if end_pos is not None and pos + 8 > end_pos:
            return None, pos
        byte_val, pos = read_bits_le(data, pos, 8)
        v |= ((byte_val >> 1) & 0x7F) << shift
        if not (byte_val & 1):
            break
        shift += 7
    return v, pos

def read_fstring(data, pos):
    """Read UE5 FString: int32 length + chars. Negative = UTF-16."""
    raw, pos = read_bits_le(data, pos, 32)
    strlen = raw if raw < 0x80000000 else raw - 0x100000000
    if strlen == 0:
        return "", pos
    if strlen < 0:
        # UTF-16
        count = min(-strlen, 4096)
        chars = []
        for _ in range(count):
            c, pos = read_bits_le(data, pos, 16)
            if c == 0:
                break
            try:
                chars.append(chr(c))
            except (ValueError, OverflowError):
                chars.append(f'\\u{c:04x}')
        return ''.join(chars), pos
    elif strlen > 0 and strlen <= 4096:
        chars = []
        for _ in range(strlen):
            c, pos = read_bits_le(data, pos, 8)
            if c == 0:
                chars.append('')
            else:
                chars.append(chr(c & 0x7F))
        return ''.join(chars).rstrip('\x00'), pos
    else:
        return None, pos  # bad length

def static_parse_name(data, pos):
    """Read FName: bIsHardcoded(1) + [PackedIndex | FString + Number]."""
    is_hardcoded = read_bit(data, pos)
    pos += 1
    if is_hardcoded:
        idx, pos = serialize_int_packed(data, pos)
        return f"EName[{idx}]", pos
    else:
        name, pos = read_fstring(data, pos)
        num, pos = read_bits_le(data, pos, 32)
        if name is None:
            return None, pos
        return name, pos

def find_content_end(data):
    """Find the last '1' bit (UE5 termination bit), return bit count."""
    for i in range(len(data) - 1, -1, -1):
        if data[i]:
            byte_val = data[i]
            bit_pos = i * 8 + 7
            while not (byte_val & 0x80):
                byte_val <<= 1
                bit_pos -= 1
            return bit_pos
    return 0

def extract_realigned(data, start_bit, num_bits):
    """Extract num_bits starting at start_bit into a new byte array, realigned."""
    result = bytearray((num_bits + 7) // 8)
    for i in range(num_bits):
        sb = start_bit + i
        byte_idx = sb >> 3
        if byte_idx < len(data):
            if (data[byte_idx] >> (sb & 7)) & 1:
                result[i >> 3] |= 1 << (i & 7)
    return bytes(result)


# ═══════════════════════════════════════════════════════════════
# Packet layer
# ═══════════════════════════════════════════════════════════════

# AoC outer header: Magic(32) + SessionID(2) + ClientID(3) + HandshakeBit(1) = 38 bits
OUTER_HDR_BITS = 38
# AoC custom field between ack history and PacketInfo
CUSTOM_FIELD_BITS = 48

def parse_packet(raw, direction):
    """
    Parse a single UE5+AoC UDP data packet.
    Returns dict with seq, ack, bunches list, or None if invalid.
    """
    outer_bits = find_content_end(raw)
    if outer_bits < OUTER_HDR_BITS + 64:
        return None

    inner_total = outer_bits - OUTER_HDR_BITS
    inner_data = extract_realigned(raw, OUTER_HDR_BITS, inner_total)
    content_bits = find_content_end(inner_data)
    if content_bits < 64:
        return None

    # ── Packed header (32 bits) ──
    packed_hdr, _ = read_bits_le(inner_data, 0, 32)
    hist_word_count = (packed_hdr & 0xF) + 1
    acked_seq = (packed_hdr >> 4) & 0x3FFF
    seq = (packed_hdr >> 18) & 0x3FFF

    # ── History words ──
    pos = 32
    history = []
    for i in range(hist_word_count):
        hw, pos = read_uint32(inner_data, pos)
        history.append(hw)

    if pos + CUSTOM_FIELD_BITS > content_bits:
        return None

    # ── AoC custom field (48 bits = 6 bytes) ──
    custom_raw, pos = read_bits_le(inner_data, pos, CUSTOM_FIELD_BITS)

    # ── PacketInfo ──
    # CRITICAL FIX: hasServerFrameTime is ALWAYS read, regardless of bHasPktInfo
    if pos >= content_bits:
        return {'seq': seq, 'ack': acked_seq, 'bunches': [], 'direction': direction}

    has_pkt_info = read_bit(inner_data, pos)
    pos += 1

    jitter_ms = 0
    if has_pkt_info:
        jitter_ms, pos = serialize_int(inner_data, pos, 1024)  # 10 bits

    # hasServerFrameTime — ALWAYS present
    has_srv_frame = read_bit(inner_data, pos)
    pos += 1

    # FrameTimeByte: S>C writes it if flag=1; C>S NEVER writes it
    if has_srv_frame and direction == 'S>C':
        _, pos = read_bits_le(inner_data, pos, 8)
# -- Cyb x C-O-A --

    # ── Bunch parsing ──
    bunches = []
    max_bunches = 64
    while pos < content_bits and len(bunches) < max_bunches:
        bunch, new_pos = parse_bunch_header(inner_data, pos, content_bits)
        if bunch is None:
            break
        bunches.append(bunch)
        pos = new_pos

    return {
        'seq': seq,
        'ack': acked_seq,
        'direction': direction,
        'has_pkt_info': has_pkt_info,
        'has_srv_frame': has_srv_frame,
        'jitter': jitter_ms,
        'bunches': bunches,
        'inner_data': inner_data,
        'content_bits': content_bits,
    }


# ═══════════════════════════════════════════════════════════════
# Bunch header parsing
# ═══════════════════════════════════════════════════════════════

MAX_PKT_BITS = 1024 * 8
MAX_CHSEQ = 1024

def parse_bunch_header(data, pos, content_bits):
    """Parse a single bunch header. Returns (bunch_dict, next_pos) or (None, pos)."""
    start = pos
    if pos >= content_bits:
        return None, pos

    b_ctrl = read_bit(data, pos); pos += 1
    b_open = 0
    b_close = 0
    close_reason = 0

    if b_ctrl:
        b_open = read_bit(data, pos); pos += 1
        b_close = read_bit(data, pos); pos += 1
        if b_close:
            close_reason, pos = serialize_int(data, pos, 15)

    # bIsReplicationPaused — ALWAYS read (UE5 reads it unconditionally)
    pos += 1

    b_reliable = read_bit(data, pos); pos += 1

    ch_idx, pos = serialize_int_packed(data, pos)
    if ch_idx > 100000:
        return None, start  # sanity

    b_has_exports = read_bit(data, pos); pos += 1
    b_has_must_map = read_bit(data, pos); pos += 1
    b_partial = read_bit(data, pos); pos += 1

    ch_seq = 0
    if b_reliable:
        ch_seq, pos = serialize_int(data, pos, MAX_CHSEQ)

    # Partial bunch fields
    partial_initial = 0
    partial_has_comp_export = 0
    partial_final = 0
    if b_partial:
        partial_initial = read_bit(data, pos); pos += 1
        partial_has_comp_export = read_bit(data, pos); pos += 1
        partial_final = read_bit(data, pos); pos += 1

    # Channel name
    ch_name = None
    if b_reliable or b_open:
        ch_name, pos = static_parse_name(data, pos)
        if ch_name is None:
            return None, start

    # BunchDataBits
    bunch_data_bits, pos = serialize_int(data, pos, MAX_PKT_BITS)

    # Sanity check
    if bunch_data_bits > (content_bits - pos + 16) or bunch_data_bits < 0:
        return None, start

    data_start = pos
    next_pos = pos + bunch_data_bits

    return {
        'ctrl': b_ctrl,
        'open': b_open,
        'close': b_close,
        'close_reason': close_reason,
        'reliable': b_reliable,
        'ch': ch_idx,
        'ch_name': ch_name or '',
        'ch_seq': ch_seq,
        'has_exports': b_has_exports,
        'has_must_map': b_has_must_map,
        'partial': b_partial,
        'partial_initial': partial_initial,
        'partial_final': partial_final,
        'bunch_data_bits': bunch_data_bits,
        'data_start': data_start,
        'hdr_bits': data_start - start,
    }, next_pos


# ═══════════════════════════════════════════════════════════════
# NetGUID export parsing (recursive InternalLoadObject)
# ═══════════════════════════════════════════════════════════════
# [C | Y | B :: CoA]

class GUIDCache:
    """Persistent GUID→path cache, like UE5's FNetGUIDCache."""
    def __init__(self):
        self.paths = {}    # guid_value → full_path_string
        self.names = {}    # guid_value → object_name (leaf)
        self.outers = {}   # guid_value → outer_guid_value
        self.dynamic = {}  # guid_value → bool

    def register(self, guid_value, name, outer_value=None, is_dynamic=False):
        if name and not name.startswith('<BAD'):
            self.names[guid_value] = name
        if outer_value is not None:
            self.outers[guid_value] = outer_value
        self.dynamic[guid_value] = is_dynamic

    def get_full_path(self, guid_value, max_depth=10):
        parts = []
        current = guid_value
        for _ in range(max_depth):
            name = self.names.get(current)
            if name:
                parts.append(name)
            outer = self.outers.get(current)
            if outer and outer != current:
                current = outer
            else:
                break
        parts.reverse()
        return '/'.join(parts) if parts else None

    def resolve(self, guid_value):
        return self.get_full_path(guid_value) or self.names.get(guid_value)


def internal_load_object(data, pos, end_pos, is_exporting, guid_cache, depth=0):
    """
    Mirror of UPackageMapClient::InternalLoadObject.
    Returns (result_dict, new_pos).
    """
    if pos + 8 > end_pos:
        return {'guid': 0, 'value': 0, 'error': 'truncated'}, pos

    net_guid, pos = serialize_int_packed64(data, pos)
    if net_guid is None:
        net_guid = 0
    is_dynamic = bool(net_guid & 1)
    guid_value = net_guid >> 1

    result = {
        'guid': net_guid,
        'value': guid_value,
        'dynamic': is_dynamic,
    }

    # NULL GUID → return immediately, NO ExportFlags
    if net_guid == 0:
        return result, pos

    # Read ExportFlags only when exporting
    if is_exporting and depth < 10:
        if pos + 8 > end_pos:
            result['error'] = 'truncated_flags'
            return result, pos

        export_flags, pos = read_uint8(data, pos)
        has_path = bool(export_flags & 0x01)
        no_load = bool(export_flags & 0x02)
        has_checksum = bool(export_flags & 0x04)

        result['flags'] = export_flags
        result['has_path'] = has_path

        if has_path:
            # Recursive: read outer GUID
            outer, pos = internal_load_object(data, pos, end_pos, True, guid_cache, depth + 1)
            result['outer'] = outer

            # Object name (FString)
            if pos + 32 <= end_pos:
                obj_name, pos = read_fstring(data, pos)
                if obj_name is None:
                    result['error'] = 'bad_name_length'
                    return result, pos
                result['name'] = obj_name

                # Register in cache
                outer_value = outer.get('value', 0) if outer.get('guid') else None
                guid_cache.register(guid_value, obj_name, outer_value, is_dynamic)
            else:
                result['error'] = 'truncated_name'
                return result, pos

            # Network checksum (only if flag set)
            if has_checksum:
                if pos + 32 <= end_pos:
                    checksum, pos = read_uint32(data, pos)
                    result['checksum'] = checksum
                else:
                    result['error'] = 'truncated_checksum'
        else:
            # No path — just the GUID reference
            pass

    return result, pos


def decode_guid_exports(data, pos, end_pos, guid_cache):
    """Decode ReceiveNetGUIDBunch: NumGUIDs + [InternalLoadObject × N]."""
    if pos + 32 > end_pos:
        return {'num': 0, 'exports': [], 'error': 'truncated'}, pos

    num_guids, pos = read_uint32(data, pos)
    if num_guids > 256:
        return {'num': num_guids, 'exports': [], 'error': 'too_many'}, pos

    exports = []
    for _ in range(num_guids):
        if pos + 8 > end_pos:
            break
        exp, pos = internal_load_object(data, pos, end_pos, True, guid_cache)
        exports.append(exp)

    return {'num': num_guids, 'exports': exports}, pos


def decode_rep_layout_exports(data, pos, end_pos):
    """Decode ReceiveNetFieldExportsCompat."""
    if pos + 32 > end_pos:
        return {'num': 0, 'entries': [], 'error': 'truncated'}, pos

    num_entries, pos = read_uint32(data, pos)
    if num_entries > 256:
        return {'num': num_entries, 'entries': [], 'error': 'too_many'}, pos

    entries = []
    for _ in range(num_entries):
        if pos + 33 > end_pos:
            break
        path_index, pos = read_uint32(data, pos)
        was_exported = read_bit(data, pos); pos += 1
        entry = {'path_index': path_index, 'exported': was_exported}

        if was_exported:
            path_name, pos = read_fstring(data, pos)
            if path_name is None:
                entry['error'] = 'bad_path'
                entries.append(entry)
                break
            entry['path'] = path_name

            if pos + 32 > end_pos:
                entries.append(entry)
                break
            num_fields, pos = read_uint32(data, pos)
            if num_fields > 512:
                entry['error'] = 'too_many_fields'
                entries.append(entry)
                break
# [C:Y:B - CoA]

            fields = []
            for _ in range(num_fields):
                if pos + 65 > end_pos:
                    break
                handle, pos = read_uint32(data, pos)
                compat_cksum, pos = read_uint32(data, pos)
                is_exported = read_bit(data, pos); pos += 1
                fld = {'handle': handle, 'checksum': compat_cksum, 'exported': is_exported}
                if is_exported:
                    fname, pos = read_fstring(data, pos)
                    if fname is None:
                        fld['error'] = 'bad_field_name'
                        fields.append(fld)
                        break
                    fld['name'] = fname
                    ftype, pos = read_uint8(data, pos)
                    fld['type'] = ftype
                fields.append(fld)
            entry['fields'] = fields
        entries.append(entry)

    return {'num': num_entries, 'entries': entries}, pos


# ═══════════════════════════════════════════════════════════════
# SerializeNewActor
# ═══════════════════════════════════════════════════════════════

def read_double_vector(data, pos):
    """Read 3 × float64 (192 bits) from bitstream."""
    components = []
    for _ in range(3):
        # Read 64 bits as bytes, then unpack as float64
        raw_bytes = bytearray(8)
        for byte_i in range(8):
            raw_bytes[byte_i], _ = read_bits_le(data, pos + byte_i * 8, 8)
        pos += 64
        val = struct.unpack('<d', raw_bytes)[0]
        components.append(round(val, 3))
    return tuple(components), pos

def read_packed_vector(data, pos, max_bits=24, scale=10):
    """Read SerializePackedVector."""
    bits, pos = serialize_int(data, pos, max_bits)
    comp_bits = bits + 2
    if comp_bits > 32 or comp_bits < 2:
        return None, pos
    bias = 1 << (bits + 1)
    x, pos = read_bits_le(data, pos, comp_bits)
    y, pos = read_bits_le(data, pos, comp_bits)
    z, pos = read_bits_le(data, pos, comp_bits)
    return (round((x - bias) / scale, 2), round((y - bias) / scale, 2), round((z - bias) / scale, 2)), pos

def read_rotation(data, pos):
    """Read FRotator::SerializeCompressedShort (3 flags + 16-bit components)."""
    flags, pos = read_bits_le(data, pos, 3)
    pitch = yaw = roll = 0.0
    if flags & 1:
        raw, pos = read_bits_le(data, pos, 16)
        pitch = raw * 360.0 / 65536.0
    if flags & 2:
        raw, pos = read_bits_le(data, pos, 16)
        yaw = raw * 360.0 / 65536.0
    if flags & 4:
        raw, pos = read_bits_le(data, pos, 16)
        roll = raw * 360.0 / 65536.0
    return (round(pitch, 2), round(yaw, 2), round(roll, 2)), pos


def decode_new_actor(data, pos, end_pos, guid_cache):
    """Decode SerializeNewActor. GUIDs are BARE (not in export mode)."""
    result = {}
    start = pos

    # Actor NetGUID (bare)
    actor_guid, pos = serialize_int_packed64(data, pos)
    if actor_guid is None:
        actor_guid = 0
    result['guid'] = actor_guid
    result['value'] = actor_guid >> 1
    result['dynamic'] = bool(actor_guid & 1)

    if not result['dynamic']:
        result['bits'] = pos - start
        return result, pos

    if pos + 8 > end_pos:
        result['error'] = 'truncated'
        result['bits'] = pos - start
        return result, pos

    # Archetype NetGUID (bare)
    archetype, pos = serialize_int_packed64(data, pos)
    if archetype is None:
        archetype = 0
    result['archetype'] = archetype
    result['archetype_value'] = archetype >> 1

    # Level NetGUID (bare, UE5.4+)
    level, pos = serialize_int_packed64(data, pos)
    if level is None:
        level = 0
    result['level'] = level
    result['level_value'] = level >> 1

    if pos >= end_pos:
        result['error'] = 'truncated_spawn'
        result['bits'] = pos - start
        return result, pos

    # bSerializeLocation
    b_loc = read_bit(data, pos); pos += 1
    result['has_location'] = bool(b_loc)
    if b_loc:
        b_quantize = read_bit(data, pos); pos += 1
        if b_quantize:
            loc, pos = read_packed_vector(data, pos)
            result['location'] = loc
        else:
            # 3 × double (192 bits) — often misaligned, tag as raw
            loc, pos = read_double_vector(data, pos)
            result['location'] = loc
            result['location_raw'] = True

    # bSerializeRotation
    b_rot = read_bit(data, pos); pos += 1
    result['has_rotation'] = bool(b_rot)
    if b_rot:
        rot, pos = read_rotation(data, pos)
        result['rotation'] = rot
# built by C | Y | B with C-O-A

    # bSerializeScale
    b_scale = read_bit(data, pos); pos += 1
    result['has_scale'] = bool(b_scale)
    if b_scale:
        b_quantize = read_bit(data, pos); pos += 1
        if b_quantize:
            sc, pos = read_packed_vector(data, pos)
            result['scale'] = sc
        else:
            sc, pos = read_double_vector(data, pos)
            result['scale'] = sc
            result['scale_raw'] = True

    # bSerializeVelocity
    b_vel = read_bit(data, pos); pos += 1
    result['has_velocity'] = bool(b_vel)
    if b_vel:
        b_quantize = read_bit(data, pos); pos += 1
        if b_quantize:
            vel, pos = read_packed_vector(data, pos)
            result['velocity'] = vel
        else:
            vel, pos = read_double_vector(data, pos)
            result['velocity'] = vel
            result['velocity_raw'] = True

    result['bits'] = pos - start
    return result, pos


# ═══════════════════════════════════════════════════════════════
# Content Block parsing
# ═══════════════════════════════════════════════════════════════

def decode_content_block(data, pos, end_pos, direction, guid_cache):
    """
    Decode one content block:
      bHasRepLayout(1) + bIsActor(1) + [sub-object GUID stuff] + NumPayloadBits + payload

    AoC custom encoding:
      bHasRepLayout=1 → NumPayloadBits encoded via SerializeIntPacked (explicit size)
      bHasRepLayout=0 → No NumPayloadBits field; payload extends to end of bunch
                         (last content block in bunch, or single-block bunch)
    """
    if pos + 2 > end_pos:
        return None, pos

    b_has_rep = read_bit(data, pos); pos += 1
    b_is_actor = read_bit(data, pos); pos += 1

    header = {'has_rep': b_has_rep, 'is_actor': b_is_actor}

    if b_is_actor:
        # Actor properties directly
        if pos >= end_pos:
            return {'header': header, 'payload_bits': 0}, pos

        if b_has_rep:
            # Explicit size via SIP
            num_payload, pos = serialize_int_packed(data, pos)
            if num_payload is None:
                num_payload = 0
            if num_payload > 100000 or num_payload > (end_pos - pos):
                return {'header': header, 'payload_bits': num_payload,
                        'error': f'bad_payload_{num_payload}'}, pos
            pos += num_payload
        else:
            # No NumPayloadBits — payload extends to end of bunch
            num_payload = end_pos - pos
            pos = end_pos

        return {'header': header, 'payload_bits': num_payload}, pos
# (CoA | C:Y:B)

    # Sub-object: bare GUID read
    sub_guid, pos = serialize_int_packed64(data, pos)
    if sub_guid is None:
        sub_guid = 0
    sub_value = sub_guid >> 1 if sub_guid else 0
    header['sub_guid'] = sub_guid
    header['sub_value'] = sub_value

    if sub_guid == 0:
        # NULL sub-object = no valid object
        if not b_has_rep:
            # bHasRep=0 means last block — consume remaining to prevent runaway loops
            num_payload = end_pos - pos
            pos = end_pos
            return {'header': header, 'payload_bits': num_payload, 'null_sub': True}, pos
        else:
            # bHasRep=1: read NumPayloadBits (might be 0) to advance past this block
            if pos < end_pos:
                num_payload, pos = serialize_int_packed(data, pos)
                if num_payload is not None and num_payload <= (end_pos - pos):
                    pos += num_payload
                elif num_payload is None:
                    num_payload = 0
                return {'header': header, 'payload_bits': num_payload or 0, 'null_sub': True}, pos
            return {'header': header, 'payload_bits': 0, 'null_sub': True}, pos

    # Valid sub-object
    if direction == 'S>C':
        if pos >= end_pos:
            # Ran out of bits after sub_guid — treat as end-of-data, not error
            return {'header': header, 'payload_bits': 0}, pos

        b_stably = read_bit(data, pos); pos += 1
        header['stably_named'] = b_stably

        if not b_stably:
            # Dynamic sub-object: read class GUID
            class_guid, pos = serialize_int_packed64(data, pos)
            class_value = class_guid >> 1 if class_guid else 0
            header['class_guid'] = class_guid
            header['class_value'] = class_value

            if class_guid == 0:
                # Destroy marker
                return {'header': header, 'payload_bits': 0, 'is_destroy': True}, pos

    # NumPayloadBits (conditional on bHasRepLayout)
    if pos >= end_pos:
        return {'header': header, 'payload_bits': 0}, pos

    if b_has_rep:
        # Explicit size via SIP
        num_payload, pos = serialize_int_packed(data, pos)
        if num_payload is None:
            num_payload = 0
        if num_payload > 100000 or num_payload > (end_pos - pos):
            return {'header': header, 'payload_bits': num_payload,
                    'error': f'bad_payload_{num_payload}'}, pos
        pos += num_payload
    else:
        # No NumPayloadBits — payload extends to end of bunch
        num_payload = end_pos - pos
        pos = end_pos

    return {'header': header, 'payload_bits': num_payload}, pos


# ═══════════════════════════════════════════════════════════════
# Full bunch decoder
# ═══════════════════════════════════════════════════════════════

def decode_bunch_data(inner_data, bunch, direction, guid_cache):
    """Decode complete bunch data: exports → must_map → new_actor → content_blocks."""
    pos = bunch['data_start']
    end_pos = pos + bunch['bunch_data_bits']
    result = {}

    # 1. Export data
    export_had_error = False
    if bunch['has_exports']:
        export_start = pos
        b_rep_layout = read_bit(inner_data, pos); pos += 1

        if b_rep_layout:
            rle, pos = decode_rep_layout_exports(inner_data, pos, end_pos)
            result['rep_layout'] = rle
            if 'error' in rle:
                export_had_error = True
        else:
            ge, pos = decode_guid_exports(inner_data, pos, end_pos, guid_cache)
            result['guid_exports'] = ge
            if 'error' in ge:
                export_had_error = True

        result['export_bits'] = pos - export_start

    pos_before_exports = bunch['data_start']
    pos_after_exports = pos  # save position after exports (before must_map)

    # 2. MustBeMappedGUIDs — try parsing, fall back to skip if it produces errors
    if bunch['has_must_map']:
        if pos + 16 <= end_pos:
            count, pos = read_uint16(inner_data, pos)
            guids = []
            for _ in range(min(count, 200)):
                if pos + 8 > end_pos:
                    break
                g, pos = serialize_int_packed64(inner_data, pos)
                guids.append(g)
            result['must_map'] = {'count': count, 'guids': guids}

    # 3. SerializeNewActor (open bunches on non-control channels)
    if bunch['open'] and bunch['ch'] > 0:
        if pos < end_pos:
            new_actor, pos = decode_new_actor(inner_data, pos, end_pos, guid_cache)
            result['new_actor'] = new_actor

    # 4. Content blocks
    blocks, block_errors, pos = _parse_content_blocks(inner_data, pos, end_pos, direction, guid_cache)

    result['blocks'] = blocks
    result['block_errors'] = block_errors
    result['bits_consumed'] = pos - bunch['data_start']
    result['bits_total'] = bunch['bunch_data_bits']
    result['bits_remaining'] = max(0, end_pos - pos)

    # 5. Fallback: if must_map parsing produced errors, retry WITHOUT must_map
    if bunch['has_must_map'] and (block_errors > 0 or result['bits_remaining'] > 2):
        pos2 = pos_after_exports  # skip must_map entirely
        # Re-do new_actor if needed
        if bunch['open'] and bunch['ch'] > 0:
            if pos2 < end_pos:
                new_actor2, pos2 = decode_new_actor(inner_data, pos2, end_pos, guid_cache)
        blocks2, errors2, pos2 = _parse_content_blocks(inner_data, pos2, end_pos, direction, guid_cache)
        remaining2 = max(0, end_pos - pos2)
        # Use the fallback if it's better (fewer errors OR same errors but less remaining)
        if (errors2 < block_errors) or (errors2 == block_errors and remaining2 < result['bits_remaining']):
            result['blocks'] = blocks2
            result['block_errors'] = errors2
            result['bits_consumed'] = pos2 - bunch['data_start']
            result['bits_remaining'] = remaining2
            result['must_map_skipped'] = True

    # 6. Fallback: if exports had error AND content blocks have errors, retry skipping exports
    if export_had_error and (result['block_errors'] > 0 or result['bits_remaining'] > 2):
        # Try from bunch start, skipping exports entirely
        pos3 = pos_before_exports
        # Skip must_map too (it's after exports and might be misaligned)
        if bunch['open'] and bunch['ch'] > 0:
            if pos3 < end_pos:
                new_actor3, pos3 = decode_new_actor(inner_data, pos3, end_pos, guid_cache)
        blocks3, errors3, pos3 = _parse_content_blocks(inner_data, pos3, end_pos, direction, guid_cache)
        remaining3 = max(0, end_pos - pos3)
        if (errors3 < result['block_errors']) or (errors3 == result['block_errors'] and remaining3 < result['bits_remaining']):
            result['blocks'] = blocks3
            result['block_errors'] = errors3
            result['bits_consumed'] = pos3 - bunch['data_start']
            result['bits_remaining'] = remaining3
            result['exports_skipped'] = True

    return result

# [CoA ~ C-Y-B]

def _parse_content_blocks(inner_data, pos, end_pos, direction, guid_cache):
    """Parse content blocks loop. Returns (blocks, block_errors, final_pos)."""
    blocks = []
    block_errors = 0
    max_blocks = 200
    while pos < end_pos and len(blocks) < max_blocks:
        # Minimum bits threshold: need at least 2 bits to start a block (HR + IA)
        if (end_pos - pos) < 3:
            break
        block, new_pos = decode_content_block(inner_data, pos, end_pos, direction, guid_cache)
        if block is None:
            break
        
        # If bad_payload error: the content block read bHasRep=1 but SIP gave garbage.
        # Try fallback: treat remaining as bHasRep=0 (consume all remaining bits).
        if 'error' in block and block.get('error', '').startswith('bad_payload_'):
            # Fallback: consume remaining bits as the last block payload
            remaining_bits = end_pos - pos
            fallback_block = {
                'header': block['header'],
                'payload_bits': remaining_bits - 2,  # minus HR+IA bits
                '_payload_fallback': True,
            }
            blocks.append(fallback_block)
            pos = end_pos
            break
        
        blocks.append(block)
        if 'error' in block:
            block_errors += 1
            break  # stop on first error
        pos = new_pos

    return blocks, block_errors, pos


# ═══════════════════════════════════════════════════════════════
# Main analysis
# ═══════════════════════════════════════════════════════════════

def load_packets(capture_file):
    """Load data packets from JSONL capture file."""
    packets = []
    with open(capture_file) as f:
        for line_no, line in enumerate(f):
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            if 'hex' not in obj or not obj['hex'] or len(obj['hex']) < 20:
                continue
            raw = bytes.fromhex(obj['hex'])
            if len(raw) < 10:
                continue
            # Check handshake bit
            is_handshake = (raw[4] >> 5) & 1
            if is_handshake:
                continue
            packets.append({
                'raw': raw,
                'dir': obj.get('dir', '?'),
                'ts': obj.get('ts', ''),
                'size': len(raw),
                'line': line_no,
            })
    return packets


def reassemble_partial_bunches(packets):
    """
    Pre-pass: find complete partial bunch chains (initial→middle*→final)
    and reassemble them into full synthetic bunches.
    
    Returns:
        reassembled: list of (synth_bunch_dict, reassembled_data_bytes, direction) 
        skip_set: set of (pkt_idx, bunch_idx_in_pkt) tuples to skip in main loop
    """
    # Collect partial bunches in order by channel
    partial_chains = defaultdict(list)  # ch → [(pkt_idx, bunch_idx, bunch, inner_data, dir)]
    
    for pkt_idx, pkt in enumerate(packets):
        parsed = parse_packet(pkt['raw'], pkt['dir'])
        if not parsed or 'bunches' not in parsed:
            continue
        for b_idx, bunch in enumerate(parsed['bunches']):
            if bunch['partial']:
                partial_chains[bunch['ch']].append({
                    'pkt_idx': pkt_idx,
                    'bunch_idx': b_idx,
                    'bunch': bunch,
                    'inner_data': parsed['inner_data'],
                    'dir': pkt['dir'],
                })
    
    # Find complete chains and reassemble
    reassembled = []
    skip_set = set()
# (C-O-A + Cyb)
    
    for ch, entries in partial_chains.items():
        current_chain = None
        for entry in entries:
            b = entry['bunch']
            if b['partial_initial']:
                if current_chain:
                    pass  # previous incomplete chain — don't skip those fragments
                if b['partial_final']:
                    # Self-contained partial (both initial+final) — not a fragment
                    current_chain = None
                    continue
                current_chain = [entry]
            elif current_chain is not None:
                current_chain.append(entry)
                if b['partial_final']:
                    # Complete chain! Reassemble it
                    total_bits = sum(e['bunch']['bunch_data_bits'] for e in current_chain)
                    reassembled_data = bytearray((total_bits + 7) // 8)
                    
                    write_pos = 0
                    for ce in current_chain:
                        cb = ce['bunch']
                        inner = ce['inner_data']
                        src_start = cb['data_start']
                        num_bits = cb['bunch_data_bits']
                        
                        for i in range(num_bits):
                            src_bit = src_start + i
                            src_byte = src_bit >> 3
                            if src_byte < len(inner):
                                bit_val = (inner[src_byte] >> (src_bit & 7)) & 1
                                if bit_val:
                                    dst_byte = write_pos >> 3
                                    reassembled_data[dst_byte] |= 1 << (write_pos & 7)
                            write_pos += 1
                    
                    # Create synthetic bunch from initial fragment's header
                    first_bunch = current_chain[0]['bunch']
                    synth_bunch = dict(first_bunch)
                    synth_bunch['bunch_data_bits'] = total_bits
                    synth_bunch['data_start'] = 0
                    synth_bunch['_reassembled'] = True
                    synth_bunch['_chain_len'] = len(current_chain)
                    # Mark as complete (not a fragment anymore)
                    synth_bunch['partial'] = True
                    synth_bunch['partial_initial'] = True
                    synth_bunch['partial_final'] = True
                    
                    direction = current_chain[0]['dir']
                    reassembled.append((synth_bunch, bytes(reassembled_data), direction))
                    
                    # Mark all fragments in this chain for skipping
                    for ce in current_chain:
                        skip_set.add((ce['pkt_idx'], ce['bunch_idx']))
                    
                    current_chain = None
            else:
                # Middle/final without initial — orphan, don't touch
                pass
        # If chain is still open at end, don't skip those fragments
    
    return reassembled, skip_set


def run_analysis(capture_file, json_output=None, summary_only=False):
    print(f"Loading {capture_file}...")
    packets = load_packets(capture_file)
    print(f"  Data packets: {len(packets)} ({sum(1 for p in packets if p['dir']=='S>C')} S>C, "
          f"{sum(1 for p in packets if p['dir']=='C>S')} C>S)")

    # Pre-pass: reassemble partial bunch chains
    reassembled_bunches, skip_set = reassemble_partial_bunches(packets)
    print(f"  Reassembled partial chains: {len(reassembled_bunches)} "
          f"(skipping {len(skip_set)} individual fragments)")

    guid_cache = GUIDCache()

    # Stats
    stats = {
        'total_packets': len(packets),
        'parsed_packets': 0,
        'total_bunches': 0,
        'open_bunches': 0,
        'close_bunches': 0,
        'export_bunches': 0,
        'partial_bunches': 0,
        'perfect_bunches': 0,
        'error_bunches': 0,
        'partial_skipped': 0,
        'reassembled_perfect': 0,
        'reassembled_error': 0,
        'total_blocks': 0,
        'guid_exports': 0,
        'rep_layout_exports': 0,
        'actors_spawned': 0,
        'dynamic_actors': 0,
    }
    channel_stats = defaultdict(lambda: {
        'bunches': 0, 'opens': 0, 'closes': 0, 'exports': 0,
        'perfect': 0, 'errors': 0, 'names': set(), 'dirs': Counter(),
        'total_bits': 0, 'consumed_bits': 0,
    })
    actor_catalog = {}  # ch → new_actor info
    export_catalog = []  # all guid exports with paths
    all_bunch_results = []
    events = []  # chronological game events for Phase 2
# ~ C Y B - CoA ~

    for pkt_idx, pkt in enumerate(packets):
        parsed = parse_packet(pkt['raw'], pkt['dir'])
        if parsed is None:
            continue
        stats['parsed_packets'] += 1

        for b_idx, bunch in enumerate(parsed['bunches']):
            stats['total_bunches'] += 1

            # Skip fragments that were reassembled into complete chains
            if (pkt_idx, b_idx) in skip_set:
                all_bunch_results.append(None)
                continue

            ch = bunch['ch']
            cs = channel_stats[ch]
            cs['bunches'] += 1
            cs['dirs'][pkt['dir']] += 1
            if bunch['ch_name']:
                cs['names'].add(bunch['ch_name'])
            cs['total_bits'] += bunch['bunch_data_bits']

            if bunch['open']:
                stats['open_bunches'] += 1
                cs['opens'] += 1
            if bunch['close']:
                stats['close_bunches'] += 1
                cs['closes'] += 1
                events.append({
                    'type': 'close', 'ch': ch, 'ts': pkt['ts'],
                    'pkt': pkt_idx, 'dir': pkt['dir'],
                })
            if bunch['has_exports']:
                stats['export_bunches'] += 1
                cs['exports'] += 1
            if bunch['partial']:
                stats['partial_bunches'] += 1

            # Skip channel 0 (control) for deep parsing
            if ch == 0:
                all_bunch_results.append(None)
                continue

            # Decode bunch data
            result = decode_bunch_data(parsed['inner_data'], bunch, pkt['dir'], guid_cache)
            all_bunch_results.append(result)

            # Track stats
            is_perfect = result['block_errors'] == 0 and result['bits_remaining'] <= 2
            has_error = result['block_errors'] > 0
            if 'new_actor' in result and 'error' in result['new_actor']:
                # new_actor overrun with remaining~0 is near-perfect (consumed ≈ data)
                na_err = result['new_actor'].get('error', '')
                if na_err == 'truncated' and result['bits_remaining'] <= 0 and result['block_errors'] == 0:
                    is_perfect = True  # accept as near-perfect
                else:
                    has_error = True

            # Detect partial fragments (not initial+final = middle, or just initial/final)
            is_partial_fragment = bunch['partial'] and not (bunch['partial_initial'] and bunch['partial_final'])
            result['_is_partial_fragment'] = is_partial_fragment

            if is_perfect:
                stats['perfect_bunches'] += 1
                cs['perfect'] += 1
            elif has_error:
                if is_partial_fragment:
                    stats['partial_skipped'] += 1
                else:
                    stats['error_bunches'] += 1
                    cs['errors'] += 1

            cs['consumed_bits'] += result['bits_consumed']

            stats['total_blocks'] += len(result.get('blocks', []))

            # GUID exports
            if 'guid_exports' in result:
                for exp in result['guid_exports'].get('exports', []):
                    stats['guid_exports'] += 1
                    export_entry = {
                        'value': exp['value'],
                        'dynamic': exp.get('dynamic', False),
                        'flags': exp.get('flags', 0),
                        'channel': ch,
                    }
                    if exp.get('name'):
                        export_entry['name'] = exp['name']
                        export_entry['path'] = guid_cache.resolve(exp['value'])
                    if exp.get('outer') and exp['outer'].get('value'):
                        export_entry['outer_value'] = exp['outer']['value']
                        if exp['outer'].get('name'):
                            export_entry['outer_name'] = exp['outer']['name']
                    export_catalog.append(export_entry)

            if 'rep_layout' in result:
                stats['rep_layout_exports'] += 1

            # New actors
            if 'new_actor' in result:
                na = result['new_actor']
                stats['actors_spawned'] += 1
                if na.get('dynamic'):
                    stats['dynamic_actors'] += 1
                loc = na.get('location')
                if na.get('location_raw'):
                    loc = None  # unreliable raw_double
                actor_info = {
                    'guid': na.get('value', 0),
                    'dynamic': na.get('dynamic', False),
                    'archetype': na.get('archetype_value'),
                    'level': na.get('level_value'),
                    'location': loc,
                    'rotation': na.get('rotation'),
                    'scale': na.get('scale') if not na.get('scale_raw') else None,
                    'velocity': na.get('velocity') if not na.get('velocity_raw') else None,
                    'archetype_path': guid_cache.resolve(na.get('archetype_value', 0)),
                    'level_path': guid_cache.resolve(na.get('level_value', 0)),
                }
                actor_catalog[ch] = actor_info
                events.append({
                    'type': 'spawn', 'ch': ch, 'ts': pkt['ts'],
                    'pkt': pkt_idx, 'dir': pkt['dir'],
                    'actor': actor_info,
                })

            # Track content block activity per channel
            if result.get('blocks'):
                events.append({
                    'type': 'replication', 'ch': ch, 'ts': pkt['ts'],
                    'pkt': pkt_idx, 'dir': pkt['dir'],
                    'num_blocks': len(result['blocks']),
                    'total_payload_bits': sum(b.get('payload_bits', 0) for b in result['blocks']),
                    'block_summary': [
                        {
                            'has_rep': b['header']['has_rep'],
                            'is_actor': b['header']['is_actor'],
                            'payload_bits': b.get('payload_bits', 0),
                            'sub_guid': b['header'].get('sub_value'),
                        }
                        for b in result['blocks']
                    ],
                })

    # ── Process reassembled partial bunches ──
    for synth_bunch, reassembled_data, direction in reassembled_bunches:
        ch = synth_bunch['ch']
        cs = channel_stats[ch]
        
        if ch == 0:
            continue
        
        result = decode_bunch_data(reassembled_data, synth_bunch, direction, guid_cache)
        all_bunch_results.append(result)
        result['_reassembled'] = True
        
        is_perfect = result['block_errors'] == 0 and result['bits_remaining'] <= 2
        has_error = result['block_errors'] > 0
        if 'new_actor' in result and 'error' in result['new_actor']:
            na_err = result['new_actor'].get('error', '')
            if na_err == 'truncated' and result['bits_remaining'] <= 0 and result['block_errors'] == 0:
                is_perfect = True
            else:
                has_error = True
# -- C.Y.B & CoA --
        
        if is_perfect:
            stats['perfect_bunches'] += 1
            stats['reassembled_perfect'] += 1
            cs['perfect'] += 1
        elif has_error:
            stats['error_bunches'] += 1
            stats['reassembled_error'] += 1
            cs['errors'] += 1
        
        cs['consumed_bits'] += result['bits_consumed']
        stats['total_blocks'] += len(result.get('blocks', []))
        
        # GUID exports from reassembled
        if 'guid_exports' in result:
            for exp in result['guid_exports'].get('exports', []):
                stats['guid_exports'] += 1
                export_entry = {
                    'value': exp['value'],
                    'dynamic': exp.get('dynamic', False),
                    'flags': exp.get('flags', 0),
                    'channel': ch,
                }
                if exp.get('name'):
                    export_entry['name'] = exp['name']
                    export_entry['path'] = guid_cache.resolve(exp['value'])
                if exp.get('outer') and exp['outer'].get('value'):
                    export_entry['outer_value'] = exp['outer']['value']
                    if exp['outer'].get('name'):
                        export_entry['outer_name'] = exp['outer']['name']
                export_catalog.append(export_entry)
        
        if 'new_actor' in result:
            na = result['new_actor']
            stats['actors_spawned'] += 1
            if na.get('dynamic'):
                stats['dynamic_actors'] += 1
            actor_catalog[ch] = {
                'guid': na.get('value', 0),
                'dynamic': na.get('dynamic', False),
                'archetype': na.get('archetype_value'),
                'level': na.get('level_value'),
                'location': na.get('location'),
                'rotation': na.get('rotation'),
                'scale': na.get('scale'),
                'velocity': na.get('velocity'),
                'archetype_path': guid_cache.resolve(na.get('archetype_value', 0)),
                'level_path': guid_cache.resolve(na.get('level_value', 0)),
            }

    # ── Print summary ──
    non_ctrl = sum(1 for b in all_bunch_results if b is not None)
    pct = (stats['perfect_bunches'] / non_ctrl * 100) if non_ctrl else 0
    print(f"\n{'='*70}")
    print(f"  PHASE 1 PARSER RESULTS")
    print(f"{'='*70}")
    print(f"  Packets parsed:     {stats['parsed_packets']}/{stats['total_packets']}")
    print(f"  Total bunches:      {stats['total_bunches']}")
    print(f"    Open:             {stats['open_bunches']}")
    print(f"    Close:            {stats['close_bunches']}")
    print(f"    Exports:          {stats['export_bunches']}")
    print(f"    Partial:          {stats['partial_bunches']}")
    print(f"  Perfect bunches:    {stats['perfect_bunches']}/{non_ctrl} ({pct:.1f}%)")
    print(f"  Error bunches:      {stats['error_bunches']}")
    if stats['partial_skipped']:
        print(f"  Partial skipped:    {stats['partial_skipped']} (fragment errors ignored)")
    if stats['reassembled_perfect'] or stats['reassembled_error']:
        print(f"  Reassembled:        {stats['reassembled_perfect']+stats['reassembled_error']} "
              f"({stats['reassembled_perfect']} perfect, {stats['reassembled_error']} errors)")
    print(f"  GUID exports:       {stats['guid_exports']}")
    print(f"  RepLayout exports:  {stats['rep_layout_exports']}")
    print(f"  Actors spawned:     {stats['actors_spawned']} ({stats['dynamic_actors']} dynamic)")
    print(f"  Unique GUID paths:  {len(guid_cache.names)}")

    # GUID path summary
    game_paths = {}
    script_paths = {}
    other_paths = {}
    for gv, name in guid_cache.names.items():
        full = guid_cache.resolve(gv) or name
        if '/Game/' in full:
            game_paths[gv] = full
        elif '/Script/' in full:
            script_paths[gv] = full
        else:
            other_paths[gv] = full

    print(f"\n  /Game/ paths:   {len(game_paths)}")
    print(f"  /Script/ paths: {len(script_paths)}")
    print(f"  Other paths:    {len(other_paths)}")

    if game_paths:
        print(f"\n  /Game/ and /Script/ paths:")
        for gv in sorted(list(game_paths.keys()) + list(script_paths.keys()))[:50]:
            path = game_paths.get(gv) or script_paths.get(gv, '?')
            print(f"    GUID {gv:>10}: {path[:100]}")

    # Channel summary
    print(f"\n{'='*70}")
    print(f"  CHANNEL SUMMARY (top 40 by bunch count)")
    print(f"{'='*70}")
    print(f"  {'Ch':>5}  {'Bnch':>5}  {'Open':>4}  {'Cls':>4}  {'Exp':>4}  "
          f"{'Perf':>5}  {'Err':>4}  {'ConsPct':>7}  {'Name':20}  {'Dir'}")
    for ch, cs in sorted(channel_stats.items(), key=lambda x: -x[1]['bunches'])[:40]:
        names = ', '.join(sorted(cs['names']))[:20] or '-'
        dirs = ' '.join(f"{d}:{c}" for d, c in cs['dirs'].items())
        cons_pct = (cs['consumed_bits'] / cs['total_bits'] * 100) if cs['total_bits'] else 0
        print(f"  {ch:>5}  {cs['bunches']:>5}  {cs['opens']:>4}  {cs['closes']:>4}  "
              f"{cs['exports']:>4}  {cs['perfect']:>5}  {cs['errors']:>4}  "
              f"{cons_pct:>6.1f}%  {names:20}  {dirs}")

    # Actor catalog
    if actor_catalog:
        print(f"\n{'='*70}")
        print(f"  ACTOR CATALOG ({len(actor_catalog)} actors, {stats['dynamic_actors']} dynamic)")
        print(f"{'='*70}")
        dynamic_actors = {ch: a for ch, a in actor_catalog.items() if a['dynamic']}
        static_actors = {ch: a for ch, a in actor_catalog.items() if not a['dynamic']}

        if dynamic_actors:
            print(f"\n  Dynamic actors ({len(dynamic_actors)}):")
            for ch, a in sorted(dynamic_actors.items())[:50]:
                arch_path = a.get('archetype_path') or f"GUID={a.get('archetype', '?')}"
                loc = a.get('location', '?')
                rot = a.get('rotation', '?')
                print(f"    Ch {ch:>4}: GUID={a['guid']:>10}  arch={arch_path[:60]}")
                if loc and loc != '?':
                    print(f"            loc={loc}  rot={rot}")
# CoA + C | Y | B

        if static_actors:
            print(f"\n  Static actors ({len(static_actors)}):")
            for ch, a in sorted(static_actors.items())[:30]:
                print(f"    Ch {ch:>4}: GUID={a['guid']:>10}")

    # Error analysis
    print(f"\n{'='*70}")
    print(f"  FIRST 20 ERRORS")
    print(f"{'='*70}")
    err_shown = 0
    for i, result in enumerate(all_bunch_results):
        if result is None:
            continue
        has_err = result.get('block_errors', 0) > 0
        if 'new_actor' in result and 'error' in result.get('new_actor', {}):
            has_err = True
        if not has_err:
            continue
        err_shown += 1
        if err_shown > 20:
            break
        rem = result['bits_remaining']
        cons = result['bits_consumed']
        tot = result['bits_total']
        frag_tag = " (partial fragment)" if result.get('_is_partial_fragment') else ""
        print(f"\n  [{i}] consumed={cons}/{tot}b remaining={rem}b{frag_tag}")
        if 'new_actor' in result and 'error' in result['new_actor']:
            print(f"    NewActor ERROR: {result['new_actor'].get('error')}")
        for j, block in enumerate(result.get('blocks', [])):
            if 'error' in block:
                print(f"    Block[{j}]: {block['error']} | hdr={block['header']}")

    # JSON output
    if json_output:
        output = {
            'stats': stats,
            'guid_names': {str(k): v for k, v in guid_cache.names.items()},
            'guid_outers': {str(k): v for k, v in guid_cache.outers.items()},
            'guid_dynamic': {str(k): v for k, v in guid_cache.dynamic.items()},
            'actor_catalog': {str(k): v for k, v in actor_catalog.items()},
            'export_catalog': export_catalog[:2000],
            'events': events,
            'channel_stats': {
                str(ch): {
                    'bunches': cs['bunches'], 'opens': cs['opens'], 'closes': cs['closes'],
                    'exports': cs['exports'], 'perfect': cs['perfect'], 'errors': cs['errors'],
                    'names': list(cs['names']),
                }
                for ch, cs in channel_stats.items()
            },
        }
        with open(json_output, 'w') as f:
            json.dump(output, f, indent=2, default=str)
        print(f"\nJSON output: {json_output}")

    print(f"\nDone.")
    return stats


if __name__ == '__main__':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

    parser = argparse.ArgumentParser(description='Phase 1: UE5/AoC Packet Parser')
    parser.add_argument('capture', nargs='?', help='JSONL capture file')
    parser.add_argument('--json', help='Output JSON file')
    parser.add_argument('--summary', action='store_true', help='Summary only')
    args = parser.parse_args()

    if args.capture:
        capture = args.capture
    else:
        # Find largest capture
        logs_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'logs')
        captures = sorted(
            [f for f in os.listdir(logs_dir) if f.startswith('capture-') and f.endswith('.jsonl')],
            key=lambda f: os.path.getsize(os.path.join(logs_dir, f)),
            reverse=True
        )
        if captures:
            capture = os.path.join(logs_dir, captures[0])
            print(f"Using largest capture: {captures[0]}")
        else:
            print("No capture files found in logs/")
            sys.exit(1)

    json_out = args.json or os.path.join(os.path.dirname(__file__), 'phase1_results.json')
    run_analysis(capture, json_output=json_out)
