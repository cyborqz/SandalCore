#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""gen_zoi_surface.py: the zone of influence (ZOI) at every point of the world, as a compact 2D table (Nodes 1, stage 1).

The ZOI chunk grid of the Cache.db export (Content/DesignAssets/World/ZOI/Verra_World_Master_ZOIs/_Generated_/Chunks) says which
ZOI owns each 512 cm cell of the world.  A ZOI is a prism: its owner is the same for every height from the first metres above the
sea to 1,638 m (the chunks of z layers 0 to 4 agree except at sea level and under ground), and the 3D lookup finds no chunk above
z 163,840.  So worldd looks a point up in this SURFACE table, which holds one owner per (x, y) cell: the cells of layer 0 at the height
of z = 16,384 cm (the same height tools' zoi_lookup.py calls the surface).

    python tools/capture/gen_zoi_surface.py                 # rewrite server/worldd/world/zoi_surface_table.h (about a minute)
    python tools/capture/gen_zoi_surface.py --check         # write nothing, exit 1 when the header would change
    python tools/capture/gen_zoi_surface.py --chunks DIR --content-db content.db

The encoding of a chunk file is the one the G1 research decoded (g1-handover research g1region/riverlands/zoi_lookup.py, validated
on all 30,558 files, the 78 ZOI records' extents and 153,359 retail player moves): the world is cut into chunks of 32,768 cm per
axis (chunk = floor(position / 32768)), each 64 x 64 x 64 cells of 512 cm; the Zen package's export (at HeaderSize, the u32 at byte 4)
is 15 header bytes, a u8 version (3), a u32 palette count P, P x u64 ZOI guid (0 = none), a u32 N and N x (UE packed-int run length,
u8 palette index); the runs walk the cells in Morton (Z-order) order, x in bit 0 of each three, y in bit 1, z in bit 2.

The header (world/zoi_surface_table.h) holds, for the 4,635 chunk columns that have a file:
  * kPalette    the ZOI guids that own a cell of the surface (0 first = no ZOI), ascending;
  * kIndex      per chunk column of the box kMinCx.., row-major (cy rows, cx columns): kNone for no file; bit 31 set = the whole chunk is one
                owner (the low byte is its palette index); else the offset in kRuns of the chunk's runs;
  * kRuns       per non-uniform chunk the runs over its 64 x 64 cells in row-major order (iy * 64 + ix), each: u16 length (little endian)
                and u8 palette index.
world/zoi_surface.h/.cpp look a point up in it.  --content-db only reports which palette guids are no ZOIRecord of that content.db (the
research found two: one under ground, one outside the Riverlands); it changes nothing in the output.  The output is deterministic and a
file keeps the line ending it already has (a new one is written with LF).
"""
import argparse
import os
import sqlite3
import struct
import sys
# Cyb and C O A

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(ROOT, 'server', 'worldd', 'world', 'zoi_surface_table.h')
DEFAULT_CHUNKS = os.environ.get('AOC_ZOI_CHUNKS', '')
CELLS = 64
CHUNK_CM = 32768
CELL_CM = 512
SURFACE_IZ = 32              # the cell row of z = 16384 cm in layer 0: 16384 // 512

# the Morton index of a cell: bit k of x, y, z go to bits 3k, 3k+1, 3k+2
_SPREAD = []
for _v in range(CELLS):
    _s = 0
    for _k in range(6):
        _s |= ((_v >> _k) & 1) << (3 * _k)
    _SPREAD.append(_s)
# the cells of the surface row, in ascending Morton order, with their row-major position (iy * 64 + ix)
_SLAB = sorted((_SPREAD[ix] | (_SPREAD[iy] << 1) | (_SPREAD[SURFACE_IZ] << 2), iy * CELLS + ix) for iy in range(CELLS) for ix in range(CELLS))


def packed(b, i):
    """FArchive::SerializeIntPacked: 7 data bits per byte in the high bits, bit 0 = another byte follows."""
    v = 0
    sh = 0
    while True:
        x = b[i]
        i += 1
        v += (x >> 1) << sh
        sh += 7
        if not (x & 1):
            return v, i


def surface_of(path):
    """(palette guids, the owner palette index of each of the 4096 cells of the surface row, row-major) of a chunk file."""
    with open(path, 'rb') as f:
        d = f.read()
    hs = struct.unpack_from('<I', d, 4)[0]
    e = d[hs:]
    if e[15] != 3:
        raise ValueError('%s: unexpected ZOI chunk version %d' % (path, e[15]))
    p = struct.unpack_from('<I', e, 16)[0]
    pal = [struct.unpack_from('<Q', e, 20 + 8 * k)[0] for k in range(p)]
    n = struct.unpack_from('<I', e, 20 + 8 * p)[0]
    i = 24 + 8 * p
    starts = []
    idx = []
    pos = 0
    for _ in range(n):
        ln, i = packed(e, i)
        starts.append(pos)
        idx.append(e[i])
        i += 1
        pos += ln
    if pos != CELLS ** 3 or i != len(e):
        raise ValueError('%s: the runs do not fill the chunk (%d cells, %d of %d bytes)' % (path, pos, i, len(e)))
    out = [0] * (CELLS * CELLS)
    j = 0
    last = len(starts) - 1
    for m, rm in _SLAB:                       # ascending Morton index, one pass over the runs
        while j < last and starts[j + 1] <= m:
            j += 1
        out[rm] = idx[j]
    return pal, out


def read_chunks(root):
    """(cx, cy) -> path of the layer 0 file, over the hash folders of the export."""
    files = {}
    for d, _dirs, names in os.walk(root):
        for f in names:
            if not f.endswith('.uasset') or not f.startswith('C_'):
                continue
            parts = f[:-7].split('_')
            if len(parts) != 4:
                continue
            cx, cy, cz = int(parts[1]), int(parts[2]), int(parts[3])
            if cz == 0:
                files[(cx, cy)] = os.path.join(d, f)
    if not files:
        sys.exit('no ZOI chunk files under %s (pass --chunks or set AOC_ZOI_CHUNKS)' % root)
    return files


def build(root):
    files = read_chunks(root)
    cols = sorted(files)
    min_cx = min(c[0] for c in cols)
    max_cx = max(c[0] for c in cols)
    min_cy = min(c[1] for c in cols)
    max_cy = max(c[1] for c in cols)
    surfaces = {}
    prov = {0: 0}            # guid -> provisional id (first seen), so a surface is one byte per cell until the palette is known
    for key in cols:
        pal, cells = surface_of(files[key])
        local = [prov.setdefault(g, len(prov)) for g in pal]
        if len(prov) > 255:
            sys.exit('more than 255 ZOIs on the surface: a palette index is a byte')
        surfaces[key] = bytes(local[i] if i < len(local) else 0 for i in cells)
    palette = sorted(prov)
    final = {g: i for i, g in enumerate(palette)}
    remap = bytes(final[g] for g, _ in sorted(prov.items(), key=lambda kv: kv[1]))
    width, height = max_cx - min_cx + 1, max_cy - min_cy + 1
    NONE = 0xFFFFFFFF
    index = [NONE] * (width * height)
    runs = bytearray()
    area = {g: 0 for g in palette}
    uniform = 0
    nruns = 0
    for key in cols:
        cells = surfaces[key].translate(remap + bytes(256 - len(remap)))
        for c in set(cells):
            area[palette[c]] += cells.count(c)
        slot = (key[1] - min_cy) * width + (key[0] - min_cx)
        if all(c == cells[0] for c in cells):
            index[slot] = 0x80000000 | cells[0]
            uniform += 1
            continue
        index[slot] = len(runs)
        k = 0
        while k < len(cells):
            m = k
            while m < len(cells) and cells[m] == cells[k]:
                m += 1
            runs += struct.pack('<HB', m - k, cells[k])
            nruns += 1
            k = m
    return dict(min_cx=min_cx, min_cy=min_cy, width=width, height=height, palette=palette, area=area, index=index, runs=bytes(runs),
                chunks=len(cols), uniform=uniform, nruns=nruns)


def rows(values, per, fmt):
    lines = []
    for i in range(0, len(values), per):
        lines.append('    ' + ', '.join(fmt % v for v in values[i:i + per]) + ',')
    return '\n'.join(lines)


def render(t):
    cell_km2 = (CELL_CM / 100000.0) ** 2
    out = ['// ============================================================================',
           '//  world/zoi_surface_table.h (Nodes 1, stage 1)',
           '//',
           '//  GENERATED by tools/capture/gen_zoi_surface.py from the ZOI chunk grid of the Cache.db export: do not edit.',
           '//  The owner (a ZOI guid) of every 512 cm cell of the world, at the surface height z = 16,384 cm; see the generator for the encoding.',
           '//  Regenerate with `python tools/capture/gen_zoi_surface.py`; scripts/generators.ps1 checks that this file is what the export gives.',
           '//',
           '//  %d chunk columns (%d one owner, %d with %d runs), %d owners (0 = no ZOI) over %d bytes of runs.' % (
               t['chunks'], t['uniform'], t['chunks'] - t['uniform'], t['nruns'], len(t['palette']), len(t['runs'])),
           '//  palette index, guid, surface km2:']
    for i, g in enumerate(t['palette']):
        out.append('//    %2d  %d  %.4f' % (i, g, t['area'][g] * cell_km2))
    out += ['// ============================================================================',
            '#pragma once',
            '',
            '#include <cstdint>',
            '',
            'namespace aoc::worldd::world::zoi_surface {',
            '',
            'inline constexpr int kChunkCm = %d;' % CHUNK_CM,
            'inline constexpr int kCellCm = %d;' % CELL_CM,
            'inline constexpr int kCells = %d;   // cells per chunk side' % CELLS,
            'inline constexpr int kMinCx = %d;' % t['min_cx'],
            'inline constexpr int kMinCy = %d;' % t['min_cy'],
            'inline constexpr int kColumns = %d;   // chunk columns in x' % t['width'],
            'inline constexpr int kRowsOfChunks = %d;   // chunk columns in y' % t['height'],
            'inline constexpr uint32_t kNone = 0xFFFFFFFFu;',
            'inline constexpr uint32_t kUniform = 0x80000000u;',
            '',
            'inline constexpr uint64_t kPalette[%d] = {' % len(t['palette']),
            rows(t['palette'], 3, '%dULL'),
            '};',
            '',
            'inline constexpr uint32_t kIndex[%d] = {' % len(t['index']),
            rows(t['index'], 8, '0x%08Xu'),
            '};',
            '',
            'inline constexpr uint8_t kRuns[%d] = {' % len(t['runs']),
            rows(list(t['runs']), 32, '%d'),
            '};',
            '',
            '}  // namespace aoc::worldd::world::zoi_surface',
            '']
    return '\n'.join(out)
# CoA - C | Y | B


def write_if_changed(path, text, check):
    crlf = False
    old = None
    if os.path.exists(path):
        with open(path, 'rb') as f:
            old = f.read()
        crlf = b'\r\n' in old
    data = text.replace('\r\n', '\n')
    if crlf:
        data = data.replace('\n', '\r\n')
    data = data.encode('utf-8')
    rel = os.path.relpath(path, ROOT)
    if old == data:
        print('%s: unchanged' % rel)
        return True
    if check:
        print('%s: OUT OF DATE (regenerate with `python tools/capture/gen_zoi_surface.py`)' % rel)
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    print('%s: updated' % rel)
    return True


def report_unknown(palette, db):
    con = sqlite3.connect('file:%s?mode=ro' % db.replace('\\', '/'), uri=True)
    try:
        known = {r[0] for r in con.execute('SELECT guid FROM city_zoi')}
    finally:
        con.close()
    unknown = [g for g in palette if g and g not in known]
    print('%d of the %d owners are no ZOIRecord of %s: %s' % (len(unknown), len(palette) - 1, os.path.basename(db), unknown))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--chunks', default=DEFAULT_CHUNKS, help='the ZOI chunk folder (default $AOC_ZOI_CHUNKS)')
    ap.add_argument('--content-db', help='report the owners that are no ZOIRecord of this content.db')
    ap.add_argument('--check', action='store_true', help='write nothing; exit 1 when the header would change')
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()
    if not args.chunks:
        sys.exit('the ZOI chunk folder is not at hand: pass --chunks or set AOC_ZOI_CHUNKS')
    t = build(args.chunks)
    if args.content_db:
        report_unknown(t['palette'], args.content_db)
    ok = write_if_changed(args.out, render(t), args.check)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
