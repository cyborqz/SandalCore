#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""gen_region_rects.py: the Riverlands as rectangles, from the game's zone of influence (ZOI) cell grid (G1, stage 1: the inputs of gen_region.py).

The world of G1 is the whole Riverlands: the zones of influence whose biome is Riverlands in the design data (content.db, table city_zoi).  Their exact border
is read out of the ZOI chunk packages of layer 0 (`mapc export-zoi` writes them from the player's own game files into dist/content/zoi_chunks; the encoding is in
gen_zoi_surface.py) at the surface height z = 16,384 cm, and cut into disjoint rectangles:

    fixtures/region/riverlands_rects.json        the disjoint rectangles [x0, y0, x1, y1) of the whole footprint (world cm, multiples of the 512 cm cell)
    fixtures/region/riverlands_zoi_rects.json    the same area per ZOI: each ZOI's record guid, name and rectangles

gen_region.py turns the two files into the header worldd reads (spawns/region_data.h) and the rectangle list mapc reads (tools/mapc/region_rects.tsv).

The cut: the chunk window cx -44..-6, cy 3..31 (the Riverlands lie inside it) is one grid of cells; a sweep along y keeps a run of cells along x open while the next row
has the very same run, and closes it as a rectangle when it does not; the sweep is made along y and along x, and the one with fewer rectangles is kept (the first, on a tie).
The order of the rectangles is the order the sweep closes them in; it is part of the output (mapc's geometry file lists the region in this order).

    python tools/capture/gen_region_rects.py                    # rewrite the two files from dist/content/zoi_chunks and dist/content/content.db
    python tools/capture/gen_region_rects.py --check            # write nothing, exit 1 when a file would change
    python tools/capture/gen_region_rects.py --chunks DIR --content-db content.db

A file keeps the line ending it has (a new one is written with LF; the JSON has no line ending at its end).
"""
import argparse
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_zoi_surface as Z  # noqa: E402  (the chunk decoder)
# Cyb x C.O.A

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RECTS_JSON = os.path.join(ROOT, 'fixtures', 'region', 'riverlands_rects.json')
ZOI_JSON = os.path.join(ROOT, 'fixtures', 'region', 'riverlands_zoi_rects.json')
DEFAULT_CHUNKS = os.environ.get('AOC_ZOI_CHUNKS') or os.path.join(ROOT, 'dist', 'content', 'zoi_chunks')
DEFAULT_DB = os.environ.get('AOC_CONTENT_DB') or os.path.join(ROOT, 'dist', 'content', 'content.db')

CELL = 512                         # cm per ZOI cell
CELLS = 64                         # cells per chunk and axis
CHUNK_CM = CELLS * CELL
XMIN, XMAX, YMIN, YMAX = -44, -6, 3, 31      # the chunk window that holds the Riverlands
REF_Z = 16384                      # the surface height of the footprint, cm
RUN = re.compile(rb'\x01+')

RECTS_NOTE = 'disjoint axis-aligned rectangles [x0,y0,x1,y1) in world cm whose union is the Riverlands surface footprint (the 1-cell hole is kept)'
ZOI_NOTE = 'disjoint [x0,y0,x1,y1) rects per Riverlands ZOI at z=16384 (the owner is the same for every z in [3072,163840))'


def load_zois(db):
    """(guid -> name of every ZOI record, the set of guids whose biome is Riverlands), from content.db's city_zoi."""
    con = sqlite3.connect('file:%s?mode=ro' % db.replace('\\', '/'), uri=True)
    try:
        rows = list(con.execute('SELECT guid, name, biome FROM city_zoi'))
    finally:
        con.close()
    return {g: n for g, n, _ in rows}, {g for g, _, b in rows if b == 'Riverlands'}


def lutb_map(lut, cells):
    """The chunk's palette indices of a row of cells as the window's ids."""
    return bytes(lut[c] if c < len(lut) else 0 for c in cells)


def read_window(root):
    """(rows, guids): rows[j] is a bytearray over the x cells of the window of the palette id (an index into guids) that owns the cell of row j, at REF_Z."""
    files = Z.read_chunks(root)
    nx, ny = (XMAX - XMIN + 1) * CELLS, (YMAX - YMIN + 1) * CELLS
    rows = [bytearray(nx) for _ in range(ny)]
    guids = [0]
    ids = {0: 0}
    for cx in range(XMIN, XMAX + 1):
        for cy in range(YMIN, YMAX + 1):
            path = files.get((cx, cy))
            if path is None:
                continue
            pal, cells = Z.surface_of(path)
            lut = []
            for g in pal:
                if g not in ids:
                    ids[g] = len(guids)
                    guids.append(g)
                lut.append(ids[g])
            if len(guids) > 255:
                sys.exit('more than 255 ZOIs in the window: a palette id is a byte')
            lutb = bytes(lut)
            x0 = (cx - XMIN) * CELLS
            for iy in range(CELLS):
                rows[(cy - YMIN) * CELLS + iy][x0:x0 + CELLS] = lutb_map(lutb, cells[iy * CELLS:(iy + 1) * CELLS])
    return rows, guids


def mask_of(rows, wanted):
    """The window as 0/1 rows: 1 where the owner id is in `wanted`."""
    table = bytes(1 if i in wanted else 0 for i in range(256))
    return [bytes(r).translate(table) for r in rows]


def decompose(rows):
    """Rectangles (i0, j0, i1, j1) of the 1 cells of rows[j][i] (half open in i and j): a run along i stays open while the next row has the very same run."""
    rects = []
    open_ = {}                       # (i0, i1) -> the row it was opened in
    for j in range(len(rows) + 1):
        runs = set()
        if j < len(rows):
            runs = set((m.start(), m.end()) for m in RUN.finditer(rows[j]))
        for r in list(open_):
            if r not in runs:
                rects.append((r[0], open_.pop(r), r[1], j))
        for r in runs:
            if r not in open_:
                open_[r] = j
    return rects


def cut(rows):
    """The rectangles in world cm [[x0, y0, x1, y1], ...]: the sweep along y or along x, whichever gives fewer."""
    along_y = decompose(rows)
    cols = [bytes(c) for c in zip(*rows)]
    along_x = [(b[1], b[0], b[3], b[2]) for b in decompose(cols)]
    use = along_y if len(along_y) <= len(along_x) else along_x
    x0, y0 = XMIN * CHUNK_CM, YMIN * CHUNK_CM
    return [[x0 + a * CELL, y0 + b * CELL, x0 + c * CELL, y0 + d * CELL] for a, b, c, d in use]



def render(rows, guids, names, riverlands):
    ids = {g: i for i, g in enumerate(guids)}
    rv_ids = {ids[g] for g in riverlands if g in ids}
    foot = mask_of(rows, rv_ids)
    cells = sum(r.count(1) for r in foot)
    rects = cut(foot)
    whole = dict(cell_cm=CELL, reference_z_cm=REF_Z, count=len(rects), area_cells=cells, rects_xy_cm=rects, note=RECTS_NOTE)
    zois = []
    for g in guids:
        if g in riverlands and g in names:
            m = mask_of(rows, {ids[g]})
            zois.append((sum(r.count(1) for r in m), g, m))
    zois.sort(key=lambda t: -t[0])       # stable: equal sizes keep the order the window met them in
    zr = [dict(name=names[g], guid=g, cells=n, rects_xy_cm=cut(m) if n else []) for n, g, m in zois]
    per_zoi = dict(cell_cm=CELL, reference_z_cm=REF_Z, note=ZOI_NOTE, zois=zr)
    return (json.dumps(whole, separators=(',', ':')), json.dumps(per_zoi, separators=(',', ':')), cells, len(rects), sum(len(z['rects_xy_cm']) for z in zr), len(zr))


def write_if_changed(path, text, check):
    old = None
    crlf = False
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
        print('%s: OUT OF DATE (regenerate with `python tools/capture/gen_region_rects.py`)' % rel)
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    print('%s: updated' % rel)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--chunks', default=DEFAULT_CHUNKS, help='the ZOI chunk folder (default $AOC_ZOI_CHUNKS, else dist/content/zoi_chunks)')
    ap.add_argument('--content-db', default=DEFAULT_DB, help='content.db for the ZOI records (default $AOC_CONTENT_DB, else dist/content/content.db)')
    ap.add_argument('--check', action='store_true', help='write nothing; exit 1 when a file would change')
    args = ap.parse_args()
    if not os.path.isdir(args.chunks):
        sys.exit('the ZOI chunk folder is not at hand: %s (mapc export-zoi writes it; or pass --chunks)' % args.chunks)
    if not os.path.isfile(args.content_db):
        sys.exit('content.db is not at hand: %s (contentc writes it; or pass --content-db)' % args.content_db)
    names, riverlands = load_zois(args.content_db)
    rows, guids = read_window(args.chunks)
    whole, per_zoi, cells, n_rects, n_zoi_rects, n_zois = render(rows, guids, names, riverlands)
    print('the Riverlands footprint: %d cells (%.4f km2) in %d rectangles; %d ZOIs in %d rectangles' % (cells, cells * CELL * CELL / 1e10, n_rects, n_zois, n_zoi_rects))
    ok = write_if_changed(RECTS_JSON, whole, args.check)
    ok = write_if_changed(ZOI_JSON, per_zoi, args.check) and ok
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
