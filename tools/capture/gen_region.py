# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""server/worldd/spawns/region_data.h and tools/mapc/region_rects.tsv: the Riverlands as data (G1, stage 1, Part A).

The world of G1 is the whole Riverlands: the 12 zones of influence (ZOIs) whose biome is Riverlands, 49.12 km^2, one connected piece.  Its exact border was read out
of the game's ZOI cell grid (the chunk files under DesignAssets/World/ZOI/Verra_World_Master_ZOIs/_Generated_/Chunks: 64 x 64 x 64 cells of 512 cm per chunk, a guid
palette and run lengths in Morton order; the decode is kept outside git with its research) and cut into disjoint rectangles, so that nothing in the repository (and
nothing at run time) reads the game's export:

    fixtures/region/riverlands_rects.json        the 401 disjoint rectangles [x0, y0, x1, y1) of the whole footprint (world cm, multiples of 512)
    fixtures/region/riverlands_zoi_rects.json    the same area per ZOI: 1,682 rectangles, each ZOI's record guid and name

Both are inputs; this script turns them into the two files the code reads:

    server/worldd/spawns/region_data.h           worldd: the rectangles, and the per-ZOI rectangles with each ZOI's guid and name (a lookup of a point's ZOI is
                                                 what the spawn filter's zone names need)
    tools/mapc/region_rects.tsv                  mapc: the 401 rectangles (it reads the cells that touch them, plus its margin)

    python tools/capture/gen_region.py            # rewrite the two files
    python tools/capture/gen_region.py --check    # write nothing, exit 1 on a change (or when the inputs contradict each other)

It needs only the two JSON files: they are in the repository, so `--check` runs from a clean checkout.  A file keeps the line ending it has (a new one is written LF).
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RECTS_JSON = os.path.join(ROOT, 'fixtures', 'region', 'riverlands_rects.json')
ZOI_JSON = os.path.join(ROOT, 'fixtures', 'region', 'riverlands_zoi_rects.json')
HEADER = os.path.join(ROOT, 'server', 'worldd', 'spawns', 'region_data.h')
MAPC_FILE = os.path.join(ROOT, 'tools', 'mapc', 'region_rects.tsv')

CELL = 512


def load():
    r = json.load(open(RECTS_JSON))
    z = json.load(open(ZOI_JSON))
    rects = [tuple(int(v) for v in q) for q in r['rects_xy_cm']]
    zois = [{'name': q['name'], 'guid': int(q['guid']), 'cells': int(q['cells']), 'rects': [tuple(int(v) for v in a) for a in q['rects_xy_cm']]} for q in z['zois']]
    return r, rects, zois


def area_cells(rects):
    return sum((x1 - x0) // CELL * ((y1 - y0) // CELL) for x0, y0, x1, y1 in rects)


def validate(r, rects, zois):
    """The inputs must agree: the counts, the grid, the disjointness, and the per-ZOI rectangles tile the same area."""
    problems = []
    if len(rects) != r['count']:
        problems.append('riverlands_rects.json: count %d but %d rectangles' % (r['count'], len(rects)))
    for q in rects:
        x0, y0, x1, y1 = q
        if not (x0 < x1 and y0 < y1) or any(v % CELL for v in q):
            problems.append('a rectangle is empty or off the %d cm grid: %s' % (CELL, q))
            break
    if area_cells(rects) != r['area_cells']:
        problems.append('the rectangles hold %d cells, the file says %d' % (area_cells(rects), r['area_cells']))
    # disjoint: rasterise the rows (cheap: 1.87 M cells) by sweeping y
    cells = set()
    for x0, y0, x1, y1 in rects:
        for cy in range(y0 // CELL, y1 // CELL):
            for cx in range(x0 // CELL, x1 // CELL):
                if (cx, cy) in cells:
                    problems.append('the rectangles overlap at cell (%d, %d)' % (cx, cy))
                    return problems
                cells.add((cx, cy))
    zcells = 0
    seen = set()
    for z in zois:
        if area_cells(z['rects']) != z['cells']:
            problems.append('%s: its rectangles hold %d cells, the file says %d' % (z['name'], area_cells(z['rects']), z['cells']))
        for x0, y0, x1, y1 in z['rects']:
            for cy in range(y0 // CELL, y1 // CELL):
                for cx in range(x0 // CELL, x1 // CELL):
                    if (cx, cy) in seen:
                        problems.append('two ZOIs overlap at cell (%d, %d)' % (cx, cy))
                        return problems
                    seen.add((cx, cy))
        zcells += z['cells']
    if seen != cells:
        problems.append('the per-ZOI rectangles do not tile the footprint: %d cells against %d (%d only in one)' % (len(seen), len(cells), len(seen ^ cells)))
    return problems
# -- Cyb x C-O-A --


def render_header(rects, zois):
    lines = []
    a = lines.append
    a('// region_data.h: the Riverlands as rectangles (G1, stage 1, Part A).')
    a('//')
    a('//  GENERATED by tools/capture/gen_region.py from fixtures/region/*.json; do not edit.')
    a('//')
    a('//  The world of G1 is the whole Riverlands: the %d zones of influence (ZOIs) whose biome is Riverlands, %.4f km^2 (%d cells of %d cm), one connected piece.' %
      (len(zois), area_cells(rects) * CELL * CELL / 1e10, area_cells(rects), CELL))
    a('//  kRiverlandsRects is the footprint as %d disjoint rectangles [x0, x1) x [y0, y1) in world cm (multiples of %d); kZoiRects holds the same area cut per ZOI' % (len(rects), CELL))
    a('//  (each kZois row names its rows by first and count).  Heights do not matter: a ZOI is a prism above sea level.  Nothing here reads the game\'s files.')
    a('#pragma once')
    a('')
    a('#include <cstddef>')
    a('#include <cstdint>')
    a('')
    a('namespace aoc::worldd::spawns::region {')
    a('')
    a('struct Rect {')
    a('    int32_t x0, y0, x1, y1;   // world cm, [x0, x1) x [y0, y1)')
    a('};')
    a('')
    a('struct ZoiRows {')
    a('    uint64_t guid;            // the ZOIRecordBase guid')
    a('    const char* name;         // its record name, e.g. Verra_RVR_StartingArea_RuinsOfAela')
    a('    uint32_t first, count;    // its rows of kZoiRects')
    a('    uint32_t cells;           // its area in cells of %d cm' % CELL)
    a('};')
    a('')
    a('inline constexpr int32_t kCellCm = %d;' % CELL)
    a('inline constexpr uint32_t kRiverlandsAreaCells = %d;' % area_cells(rects))
    a('')
    a('inline constexpr Rect kRiverlandsRects[] = {')
    for x0, y0, x1, y1 in rects:
        a('    {%d, %d, %d, %d},' % (x0, y0, x1, y1))
    a('};')
    a('inline constexpr size_t kRiverlandsRectCount = sizeof(kRiverlandsRects) / sizeof(kRiverlandsRects[0]);')
    a('')
    a('inline constexpr Rect kZoiRects[] = {')
    first = 0
    rows = []
    for z in zois:
        a('    // %s (%d rectangles)' % (z['name'], len(z['rects'])))
        for x0, y0, x1, y1 in z['rects']:
            a('    {%d, %d, %d, %d},' % (x0, y0, x1, y1))
        rows.append((z, first, len(z['rects'])))
        first += len(z['rects'])
    a('};')
    a('inline constexpr size_t kZoiRectCount = sizeof(kZoiRects) / sizeof(kZoiRects[0]);')
    a('')
    a('inline constexpr ZoiRows kZois[] = {')
    for z, f, n in rows:
        a('    {%dull, "%s", %d, %d, %d},' % (z['guid'], z['name'], f, n, z['cells']))
    a('};')
    a('inline constexpr size_t kZoiCount = sizeof(kZois) / sizeof(kZois[0]);')
    a('')
    a('}  // namespace aoc::worldd::spawns::region')
    return '\n'.join(lines) + '\n'


def render_mapc(r, rects):
    lines = ['# region_rects v1: the Riverlands footprint as disjoint rectangles, world cm, [x0,x1) x [y0,y1) (tools/mapc reads it; GENERATED by tools/capture/gen_region.py, do not edit)',
             '# cells\t%d\tcell_cm\t%d\tcount\t%d' % (area_cells(rects), CELL, len(rects))]
    for x0, y0, x1, y1 in rects:
        lines.append('rect\t%d\t%d\t%d\t%d' % (x0, y0, x1, y1))
    return '\n'.join(lines) + '\n'


def write_if_changed(path, text, check):
    old = open(path, newline='').read() if os.path.exists(path) else None
    same = old is not None and old.replace('\r\n', '\n') == text
    name = os.path.relpath(path, ROOT).replace('\\', '/')
    if check:
        print('%s: %s' % (name, 'unchanged' if same else 'WOULD CHANGE'))
        return same
    if same:
        print('%s: unchanged' % name)
        return True
    crlf = old is not None and '\r\n' in old
    with open(path, 'w', newline='') as f:
        f.write(text.replace('\n', '\r\n') if crlf else text)
    print('%s: updated' % name)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    r, rects, zois = load()
    problems = validate(r, rects, zois)
    for p in problems:
        print('PROBLEM: ' + p)
    if problems:
        sys.exit(1)
    print('%d rectangles, %d cells = %.4f km^2; %d ZOIs with %d rectangles' % (len(rects), area_cells(rects), area_cells(rects) * CELL * CELL / 1e10, len(zois), sum(len(z['rects']) for z in zois)))
    ok = write_if_changed(HEADER, render_header(rects, zois), a.check)
    ok = write_if_changed(MAPC_FILE, render_mapc(r, rects), a.check) and ok
    if a.check and not ok:
        sys.exit(1)


if __name__ == '__main__':
    main()
