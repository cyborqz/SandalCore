#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""
PCAP-to-JSONL converter for AOC captures.

Manually parses libpcap (.pcap) and pcapng (.pcapng) files without scapy/libpcap.
Extracts UDP payloads, classifies S>C vs C>S by port, and emits JSONL compatible
with phase1_parser.load_packets().

PCAP file format:
  Global header: 24 bytes
    magic_number (4): 0xa1b2c3d4 (little-endian) or 0xd4c3b2a1 (big-endian-on-disk)
                      0xa1b23c4d (nanosecond resolution)
    version_major (2), version_minor (2)
    thiszone (4), sigfigs (4)
    snaplen (4), network (4)  [network=1 = Ethernet]

  Per packet: 16-byte header
    ts_sec (4), ts_usec (4)
    incl_len (4), orig_len (4)
  Then incl_len bytes of frame data.

  Frame: Ethernet (14) [src(6) + dst(6) + ethertype(2)] -> IPv4 (20+) -> UDP (8) -> payload

Pcapng (.pcapng) file format:
  A sequence of blocks. Each block:
    block_type (4)
    block_total_length (4)
    block_body (variable, padded to 4-byte boundary)
    block_total_length (4) [redundant tail]

  Block types we care about:
    0x0A0D0D0A: Section Header Block (SHB)
    0x00000001: Interface Description Block (IDB) -> snaplen, link_type
    0x00000006: Enhanced Packet Block (EPB) -> ts_high, ts_low, captured_len, packet_len, data
    0x00000003: Simple Packet Block
"""
import json, sys, argparse, os, struct
from datetime import datetime, timezone


def parse_ethernet_ip_udp(frame):
    """Parse Ethernet -> IPv4 -> UDP. Returns (src_ip, dst_ip, src_port, dst_port, payload) or None."""
    if len(frame) < 14:
        return None
    eth_dst = frame[0:6]
    eth_src = frame[6:12]
    ethertype = struct.unpack('>H', frame[12:14])[0]
# [Cyb :: C.O.A]

    # Skip VLAN tag if present
    off = 14
    if ethertype == 0x8100:  # 802.1Q VLAN
        if len(frame) < 18:
            return None
        ethertype = struct.unpack('>H', frame[16:18])[0]
        off = 18

    if ethertype != 0x0800:  # IPv4 only
        return None
    if len(frame) < off + 20:
        return None

    ip_hdr = frame[off:off+20]
    ihl = (ip_hdr[0] & 0x0F) * 4
    if ihl < 20:
        return None
    proto = ip_hdr[9]
    if proto != 17:  # UDP only
        return None
    src_ip = '.'.join(str(b) for b in ip_hdr[12:16])
    dst_ip = '.'.join(str(b) for b in ip_hdr[16:20])

    udp_off = off + ihl
    if len(frame) < udp_off + 8:
        return None
    src_port, dst_port, udp_len, _ = struct.unpack('>HHHH', frame[udp_off:udp_off+8])
    payload = frame[udp_off+8 : udp_off + udp_len if udp_len >= 8 else len(frame)]
    return (src_ip, dst_ip, src_port, dst_port, payload)


def parse_pcap(path):
    """Parse classic libpcap. Yields (ts_sec, ts_usec, frame_bytes)."""
    with open(path, 'rb') as f:
        magic = f.read(4)
        if not magic:
            return
        # libpcap magic conventions:
        #   On disk \xd4\xc3\xb2\xa1 -> reads as 0xa1b2c3d4 little-endian -> LE file
        #   On disk \xa1\xb2\xc3\xd4 -> reads as 0xd4c3b2a1 little-endian -> BE file
        #   \x4d\x3c\xb2\xa1 -> ns precision, LE
        #   \xa1\xb2\x3c\x4d -> ns precision, BE
        if magic == b'\xd4\xc3\xb2\xa1':
            endian = '<'
            nano = False
        elif magic == b'\xa1\xb2\xc3\xd4':
            endian = '>'
            nano = False
        elif magic == b'\x4d\x3c\xb2\xa1':
            endian = '<'
            nano = True
        elif magic == b'\xa1\xb2\x3c\x4d':
            endian = '>'
            nano = True
        else:
            raise ValueError(f"Not a pcap file (magic={magic.hex()})")

        # Skip remainder of global header (20 bytes after magic)
        f.read(20)

        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                return
            ts_sec, ts_frac, incl_len, orig_len = struct.unpack(f'{endian}IIII', hdr)
            data = f.read(incl_len)
            if len(data) < incl_len:
                return
            ts_usec = ts_frac if not nano else ts_frac // 1000
            yield (ts_sec, ts_usec, data)


def parse_pcapng(path):
    """Parse pcapng. Yields (ts_sec, ts_usec, frame_bytes)."""
    with open(path, 'rb') as f:
        # Section Header Block must come first
        head = f.read(12)
        if len(head) < 12:
            return
        block_type, block_len, byte_order_magic = struct.unpack('<III', head)
        if block_type != 0x0A0D0D0A:
            raise ValueError("Not a pcapng file")
        endian = '<' if byte_order_magic == 0x1A2B3C4D else '>'
        # Re-read length with correct endian
        block_len = struct.unpack(f'{endian}I', head[4:8])[0]

        # Skip rest of SHB body + trailing length
        rest = block_len - 12
        f.read(rest)

        # Track interface time resolution (default 10^6 = microseconds)
        if_tsresol = 6  # default: microseconds

        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                return
            block_type, block_len = struct.unpack(f'{endian}II', hdr)
            body_len = block_len - 12
            body = f.read(body_len)
            tail = f.read(4)  # redundant length
            if len(body) < body_len:
                return
# (C | Y | B | C-O-A)

            if block_type == 0x00000001:
                # Interface Description Block: link_type(2) + reserved(2) + snaplen(4) + options
                # Default tsresol is 10^6 (us) unless option says otherwise
                pass
            elif block_type == 0x00000006:
                # Enhanced Packet Block
                # Body: interface_id(4) + ts_high(4) + ts_low(4) + cap_len(4) + pkt_len(4) + data + options
                if body_len < 20:
                    continue
                iface_id, ts_high, ts_low, cap_len, pkt_len = struct.unpack(f'{endian}IIIII', body[0:20])
                ts_full = (ts_high << 32) | ts_low
                ts_sec = ts_full // 1_000_000
                ts_usec = ts_full % 1_000_000
                data_start = 20
                data = body[data_start : data_start + cap_len]
                yield (ts_sec, ts_usec, data)
            elif block_type == 0x00000003:
                # Simple Packet Block: pkt_len(4) + data
                pkt_len = struct.unpack(f'{endian}I', body[0:4])[0]
                data = body[4:4 + pkt_len]
                yield (0, 0, data)
            # else: other block types ignored


def detect_direction(src_port, dst_port, server_port_hint=None):
    """Classify direction. AOC servers in these captures use port 7372."""
    SERVER_PORTS = {7372, 7777, 7778, 7779, 7780, 7781, 7782, 7783, 7784, 7785, 7786}
    if server_port_hint:
        SERVER_PORTS.add(server_port_hint)
    if src_port in SERVER_PORTS:
        return 'S>C'
    if dst_port in SERVER_PORTS:
        return 'C>S'
    # Heuristic
    if 7000 <= src_port <= 9000 and not (7000 <= dst_port <= 9000):
        return 'S>C'
    if 7000 <= dst_port <= 9000 and not (7000 <= src_port <= 9000):
        return 'C>S'
    if src_port < dst_port:
        return 'S>C'
    return 'C>S'


def convert(pcap_path, jsonl_path):
    """Convert a pcap/pcapng file to JSONL."""
    is_ng = pcap_path.lower().endswith('.pcapng')
    parser = parse_pcapng if is_ng else parse_pcap

    written = 0
    server_ips = set()
    server_ports = set()
    direction_counts = {'S>C': 0, 'C>S': 0}
    pkt_idx = 0

    try:
        with open(jsonl_path, 'w') as out:
            for ts_sec, ts_usec, frame in parser(pcap_path):
                pkt_idx += 1
                if not frame:
                    continue
                parsed = parse_ethernet_ip_udp(frame)
                if not parsed:
                    continue
                src_ip, dst_ip, src_port, dst_port, payload = parsed
                if len(payload) < 5:
                    continue

                direction = detect_direction(src_port, dst_port)
                if direction == 'S>C':
                    server_ips.add(src_ip)
                    server_ports.add(src_port)
                else:
                    server_ips.add(dst_ip)
                    server_ports.add(dst_port)
                direction_counts[direction] += 1

                ts = datetime.fromtimestamp(ts_sec + ts_usec / 1_000_000.0,
                                            tz=timezone.utc).isoformat()
                obj = {
                    'ts': ts,
                    'dir': direction,
                    'src': f"{src_ip}:{src_port}",
                    'dst': f"{dst_ip}:{dst_port}",
                    'hex': payload.hex(),
                    'size': len(payload),
                    'pkt_idx': pkt_idx,
                }
                out.write(json.dumps(obj) + '\n')
                written += 1
    except Exception as e:
        print(f"Error after {written} packets: {e}", file=sys.stderr)
        raise

    print(f"  Total UDP packets: {written}")
    print(f"  Server IPs: {server_ips}")
    print(f"  Server ports: {server_ports}")
    print(f"  Direction: S>C={direction_counts['S>C']}  C>S={direction_counts['C>S']}")
    return written


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('pcap', help='input pcap/pcapng file')
    ap.add_argument('jsonl', help='output JSONL path')
    args = ap.parse_args()
    convert(args.pcap, args.jsonl)
