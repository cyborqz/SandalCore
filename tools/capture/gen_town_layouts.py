# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""dist/content/town_layout_items.tsv: what the five towns' layouts place in a bowl, as a list of placed meshes and level instances.

The five city nodes with a platform (Winstead, Joeva, New Aela, Halcyon, Miraleth) are built by the client from their layout record:

    CityNodeRecord.LayoutId -> LayoutRecord.Props[i]   (a NodeAssetSet and a transform in the node's frame)
                               LayoutRecord.PlotsPerNodeLevel[level]   (the pre-built plots: a building's asset sets for its state)
        -> NodeAssetSet.ContainedAssetDefinitions[j]   (a NodeAssetDefinition and a transform)
        -> NodeAssetDefinition.ContainedAssets         (an asset path and its InstanceData[k]: a type, a transform, a JSON string)

and the definitions are filtered by the node's level, culture, type, season and biome (the same filter worldd's scenery sends, server/worldd/world/node_layout_data.h).
The transforms compose as  node record * item * set member * instance  (the layouts' props share the platform's frame: the record's x, y, z and yaw; the props of
Joeva's Town stand on the platform's heights to within 20 cm for four in ten of them and a shift of a metre in any direction loses that).

This writes the instances of the two kinds that have a collision, in world cm:  M  a static mesh instance (the asset is the mesh), L  a level instance (the asset is a
world or a Blueprint that places one; mapc town-layouts reads its static meshes).  Everything else the layouts place (foliage types, lights, decals, particle systems, signs,
sounds) has no pawn collision here and is left out.  Each bowl is listed at the level its node starts at, the one the server's seed file gives it (deploy/citynode_seed.json: a
node the file leaves out, and every node when there is no file, starts at level 0): the rows of the other levels are left out.  An instance that stands at several levels is one
row with a bit per level (`levels`), and a row is kept when it has the bit of its node's level.  The walking map is built once and not swapped when a node changes level, so a
level change in the game keeps the map of the level the node started at.  The mayor's buildings, the expansions and the workshops of a seed are not in it (a fresh server
does not know them).

It reads dist/content/content.db (--content-db), mode=ro, and the seed file (--seed), and nothing else:

    python tools/capture/gen_town_layouts.py [--content-db PATH] [--seed PATH] [--out PATH]            # write the file
    python tools/capture/gen_town_layouts.py [--content-db PATH] [--seed PATH] [--out PATH] --check    # write nothing, exit 1 when the file differs

File (UTF-8, LF, tab separated; mapc/TownLayouts.cs reads it):
    #...                                     comment lines
    @node   guid  name  layout  type  x  y  z  yaw         a bowl: the node record's place (cm, degrees)
    @asset  id  M|L  object path                           an asset, numbered from 0
    node  asset  levels  stepup  m00 m01 m02 m10 m11 m12 m20 m21 m22 tx ty tz
        node: a guid of an @node line; levels: hex, bit n = the instance stands at node level n; stepup: 1 when the instance lets a character step up onto it (0: no);
        the twelve numbers: world = R * local + t, the instance's scale inside R (the node record's yaw, the layout, set and instance transforms all applied).
"""
import argparse
import gzip
import json
import math
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
DEFAULT_DB = os.path.join(ROOT, 'dist', 'content', 'content.db')
DEFAULT_OUT = os.path.join(ROOT, 'dist', 'content', 'town_layout_items.tsv')
DEFAULT_SEED = os.path.join(ROOT, 'deploy', 'citynode_seed.json')

# The five bowls (server/worldd/world/town_platform.h kTownBowls): the node records by guid.
BOWLS = [108646108518008, 108646121231993, 108646608033403, 108646610851452, 108646615701117]
LEVELS = range(7)

# What the scenery sends (server/worldd/world/node_scenery.h): Kaelar, spring, and the biome of the node's zone of influence.
CULTURE = 0
SEASON = 1
RACES = ['Kaelar', 'Vaelune', 'Vek', 'Renkai', 'Empyrean', 'Pyrai', 'Dunir', 'Nikua', 'Tulnar']
SEASONS = ['Winter', 'Spring', 'Summer', 'Fall']
NODE_TYPES = ['Default', 'Scientific', 'Divine', 'Martial', 'Economic']
BIOMES = ('Default AlpineForest AncientForest ArcticTundra Asian Badlands Desert Fjords Glacier Grasslands Iceland Jungle MagicalForest RedwoodForest Riverlands Swamp '
          'Taiga TabletopMountains Tropics Volcano UnderRealm Highlands').split()
STATES = ['Empty', 'UnderConstruction', 'Operational', 'Disarray', 'Damaged', 'Repairing', 'Destroyed', 'Demolition']
STATE_LIST = {1: 'ConstructionAssetSetIds', 2: 'OperationalAssetSetIds', 3: 'DisarrayAssetSetIds', 4: 'DamagedAssetSetIds', 5: 'RepairingAssetSetIds',
              6: 'DestroyedAssetSetIds', 7: 'DemolitionAssetSetIds'}
STEP_RX = re.compile(r'canCharacterStepUpOn"\s*:\s*"(\w+)"')


SCHEMAS = ('CityNodeRecordBase', 'LayoutRecordBase', 'NodeAssetSetRecordBase', 'NodeAssetDefinitionRecordBase', 'BuildingNodeAssetSetsRecordBase',
           'ServiceBuildingPlotRecordBase', 'BuildingRecordBase')


class Records:
    """The design records the layouts need, from content.db's record table (a guid is not indexed: one scan by schema reads them all, 18 MB packed)."""

    def __init__(self, path):
        self.db = sqlite3.connect('file:' + path.replace('\\', '/') + '?mode=ro', uri=True)
        self.blobs = {}
        marks = ','.join('?' * len(SCHEMAS))
        for guid, schema, name, data in self.db.execute('SELECT guid, schema, name, data FROM record WHERE schema IN (%s)' % marks, SCHEMAS):
            self.blobs[guid] = (schema, name, data)
        self.cache = {}

    def raw(self, guid):
        """(schema, name, parsed JSON) without keeping the JSON."""
        b = self.blobs.get(guid)
        return None if b is None else (b[0], b[1], json.loads(gzip.decompress(b[2])))

    def get(self, guid):
        """The same, kept (for the small records; a definition is read through Layouts.definition)."""
        if guid not in self.cache:
            self.cache[guid] = self.raw(guid)
        return self.cache[guid]


def matrix(t):
    """A design Transform ({Rotation: FQuat, Translation, Scale3D}) as an affine map: (r00 r01 r02 r10 r11 r12 r20 r21 r22 tx ty tz), world = R * local + t, the scale inside R."""
    r = t.get('Rotation', {})
    s = t.get('Scale3D', {})
    p = t.get('Translation', {})
    x, y, z, w = r.get('X', 0.0), r.get('Y', 0.0), r.get('Z', 0.0), r.get('W', 1.0)
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    sx, sy, sz = s.get('X', 1.0), s.get('Y', 1.0), s.get('Z', 1.0)
    return ((1 - 2 * (y * y + z * z)) * sx, 2 * (x * y - z * w) * sy, 2 * (x * z + y * w) * sz,
            2 * (x * y + z * w) * sx, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z - x * w) * sz,
            2 * (x * z - y * w) * sx, 2 * (y * z + x * w) * sy, (1 - 2 * (x * x + y * y)) * sz,
            p.get('X', 0.0), p.get('Y', 0.0), p.get('Z', 0.0))


def mul(a, b):
    """a applied after b: (mul(a, b))(p) = a(b(p))."""
    return (a[0] * b[0] + a[1] * b[3] + a[2] * b[6], a[0] * b[1] + a[1] * b[4] + a[2] * b[7], a[0] * b[2] + a[1] * b[5] + a[2] * b[8],
            a[3] * b[0] + a[4] * b[3] + a[5] * b[6], a[3] * b[1] + a[4] * b[4] + a[5] * b[7], a[3] * b[2] + a[4] * b[5] + a[5] * b[8],
            a[6] * b[0] + a[7] * b[3] + a[8] * b[6], a[6] * b[1] + a[7] * b[4] + a[8] * b[7], a[6] * b[2] + a[7] * b[5] + a[8] * b[8],
            a[0] * b[9] + a[1] * b[10] + a[2] * b[11] + a[9], a[3] * b[9] + a[4] * b[10] + a[5] * b[11] + a[10], a[6] * b[9] + a[7] * b[10] + a[8] * b[11] + a[11])


def frame_matrix(x, y, z, yaw_degrees):
    """The node record's frame: world = record + Rz(yaw) * local (UE's yaw turns +x towards +y)."""
    a = math.radians(yaw_degrees)
    c, s = math.cos(a), math.sin(a)
    return (c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0, x, y, z)


FILTER_KEYS = ('NodeLevelMinimum', 'NodeLevelMaximum', 'ValidCultures', 'ValidNodeTypes', 'ValidSeasons', 'bAllBiomes', 'ValidBiomes')


def def_valid(d, level, ntype):
    """Whether a definition (its filter fields) holds for a node of `ntype` at `level` in the Riverlands, for Kaelar in spring."""
    if not (d.get('NodeLevelMinimum', 0) <= level <= d.get('NodeLevelMaximum', 6)):
        return False
    if 'ECharacterRace::' + RACES[CULTURE] not in d.get('ValidCultures', []):
        return False
    if 'ENodeType::' + NODE_TYPES[ntype] not in d.get('ValidNodeTypes', []):
        return False
    if 'ESeasons::' + SEASONS[SEASON] not in d.get('ValidSeasons', []):
        return False
    return bool(d.get('bAllBiomes')) or 'EBiomeType::Riverlands' in d.get('ValidBiomes', [])


class Layouts:
    def __init__(self, records):
        self.rec = records
        self.defs = {}      # definition guid -> parsed assets (None: no such record)
        self.valid = {}

    def definition(self, guid):
        """The definition's asset instances of the two kinds with a collision: [(kind, path, (n,4,4) matrices, stepup flags)], and its filter."""
        if guid in self.defs:
            return self.defs[guid]
        r = self.rec.raw(guid)
        if r is None:
            self.defs[guid] = None
            return None
        d = r[2]
        assets = []
        for a in d.get('ContainedAssets', []):
            key = a.get('Key', '')
            is_world = '/Level_Instances/' in key or key.startswith('/Game/Levels/NodeLayouts')
            mats, steps = [], []
            kind = None
            for inst in a.get('Value', {}).get('InstanceData', []):
                ty = inst.get('Type', '').split('::')[-1]
                if ty == 'StaticMesh':
                    k = 'M'
                elif is_world:
                    k = 'L'
                else:
                    continue
                if kind is None:
                    kind = k
                elif kind != k:
                    raise SystemExit('gen_town_layouts: %s mixes static meshes and level instances (%s)' % (key, r[1]))
                mats.append(matrix(inst.get('Transform', {})))
                m = STEP_RX.search(inst.get('DataString', '') or '')
                steps.append(0 if m and m.group(1) == 'ECB_No' else 1)
            if kind:
                assets.append((kind, key, mats, steps))
        out = {'name': r[1], 'filter': {k: d[k] for k in FILTER_KEYS if k in d}, 'assets': assets}
        self.defs[guid] = out
        return out

    def valid_defs(self, set_guid, level, ntype):
        """The set's definitions that hold for the node at `level`: [(definition guid, member transform)]."""
        s = self.rec.get(set_guid)
        if s is None:
            return []
        out = []
        for cd in s[2].get('ContainedAssetDefinitions', []):
            g = cd.get('Asset', {}).get('Guid', 0)
            d = self.definition(g)
            if d is None or not def_valid(d['filter'], level, ntype):
                continue
            out.append((g, cd.get('Transform', {})))
        return out

    def items(self, node_rec, level):
        """The (item transform, asset set) pairs of the node at `level`, as the scenery sends them: the layout's props, then the pre-built plots."""
        d = node_rec[2]
        ntype = NODE_TYPES.index(d['NodeType'].split('::')[1])
        lay = self.rec.get(d['LayoutId']['Guid'])
        out = []
        for p in lay[2].get('Props', []):
            sg = p['AssetSet']['Guid']
            if self.set_valid(sg, level, ntype):
                out.append((p['Transform'], sg))
        plots = []
        for e in lay[2].get('PlotsPerNodeLevel', []):
            if e['Key'] == level:
                plots = e['Value']['Plots']
        for pl in plots:
            pr = self.rec.get(pl['Plot']['Guid'])
            pd = pr[2]
            if not pd.get('PreConstruct'):
                continue    # nobody built on it: it sends nothing
            state = STATES.index(pd['StartingState'].split('::')[1])
            ids = [b['Guid'] for b in pd.get('BuildingsIds', []) if b['Guid']]
            bg = ids[0] if len(ids) == 1 else pd.get('EmptyBuildingId', {}).get('Guid', 0)
            if not bg and ids:
                bg = ids[0]
            if not bg:
                continue
            # this world starts every pre-built plot Operational, the Large project excepted (node_wire.cpp)
            st = state if 'ConstructionProject_Large' in pr[1] else 2
            b = self.rec.get(bg)
            sets = self.rec.get(b[2]['BuildingAssetSetId']['Guid'])
            for sg in [x['Guid'] for x in sets[2].get(STATE_LIST.get(st, ''), [])]:
                if self.set_valid(sg, level, ntype):
                    out.append((pl['Transform'], sg))
        return out, ntype

    def set_valid(self, set_guid, level, ntype):
        key = (set_guid, level, ntype)
        v = self.valid.get(key)
        if v is None:
            v = len(self.valid_defs(set_guid, level, ntype)) > 0
            self.valid[key] = v
        return v


def norm(name):
    """A node name with everything but letters and digits taken out, in lower case: "New Aela", "NewAela" and "new_aela" are one name (the server's seed file reads names the same way)."""
    return re.sub(r'[^0-9a-z]', '', name.lower())


def seed_levels(path, nodes):
    """{bowl guid: level}: the level each bowl starts at in the server's seed file.  The file names a node by its record name (Verra_RVR_Joeva), the same without Verra_, its display name
    (Joeva) or its guid; a bowl it leaves out starts at level 0 (Wilderness), and so does every bowl when there is no file.  Other nodes of the file are not bowls and are ignored."""
    levels = {n['guid']: 0 for n in nodes}
    if not path or not os.path.exists(path):
        return levels
    with open(path, 'rb') as f:
        seed = json.loads(f.read().decode('utf-8-sig'))
    by_name = {}
    for n in nodes:
        keys = {norm(n['name']), norm(n['node_name']), str(n['guid'])}
        if n['name'].startswith('Verra_'):
            keys.add(norm(n['name'][len('Verra_'):]))
        for k in keys:
            by_name[k] = n['guid']
    for name, entry in (seed.get('nodes') or {}).items():
        guid = by_name.get(norm(name))
        if guid is None:
            continue
        level = entry.get('level') if isinstance(entry, dict) else None
        if not isinstance(level, int) or isinstance(level, bool) or not 0 <= level < len(LEVELS):
            raise SystemExit('gen_town_layouts: %s: node "%s" needs a level from 0 to %d' % (path, name, len(LEVELS) - 1))
        levels[guid] = level
    return levels


def num(v, digits):
    s = ('%.' + str(digits) + 'f') % v
    if s.startswith('-') and float(s) == 0:
        s = s[1:]
    return s.rstrip('0').rstrip('.') if '.' in s else s


def rotnum(v):
    s = '%.9g' % v
    return '0' if float(s) == 0 else s


def build(db_path, seed_path=None):
    rec = Records(db_path)
    lay = Layouts(rec)
    zoi_biome = {}
    for row in rec.db.execute('SELECT node_guid, biome_ord FROM city_zoi'):
        zoi_biome[row[0]] = row[1]
    nodes = []
    for guid in BOWLS:
        row = rec.db.execute('SELECT guid, name, x, y, z, yaw, layout_guid, node_name FROM city_node WHERE guid = ?', (guid,)).fetchone()
        if row is None:
            raise SystemExit('gen_town_layouts: city_node has no record %d (content.db is too old?)' % guid)
        nr = rec.get(guid)
        layout_name = rec.get(row[6])[1]
        nodes.append({'guid': guid, 'name': row[1], 'node_name': row[7], 'x': row[2], 'y': row[3], 'z': row[4], 'yaw': row[5], 'layout': layout_name, 'rec': nr})
        biome = zoi_biome.get(guid)
        if biome is not None and BIOMES[biome] != 'Riverlands':
            raise SystemExit('gen_town_layouts: %s is not a Riverlands node (biome %s)' % (row[1], BIOMES[biome]))
    levels = seed_levels(seed_path, nodes)
    for n in nodes:
        n['level'] = levels[n['guid']]
    asset_ids = {}
    rows = []       # (node guid, asset key, kind, levels mask, stepup, 4x4)
    stats = {}
    for n in nodes:
        frame = frame_matrix(n['x'], n['y'], n['z'], n['yaw'])
        merged = {}     # (asset, rounded matrix) -> [mask, stepup, matrix, kind]
        for level in LEVELS:
            items, ntype = lay.items(n['rec'], level)
            n['type'] = NODE_TYPES[ntype]
            for tr, sg in items:
                item = mul(frame, matrix(tr))
                for dg, mtr in lay.valid_defs(sg, level, ntype):
                    d = lay.definition(dg)
                    base = mul(item, matrix(mtr))
                    for kind, key, mats, steps in d['assets']:
                        for i, inst in enumerate(mats):
                            m = mul(base, inst)
                            k = (key, tuple(int(round(v * 1e6)) for v in m[:9]), tuple(int(round(v * 1e3)) for v in m[9:]))
                            e = merged.get(k)
                            if e is None:
                                merged[k] = [1 << level, steps[i], m, kind]
                            else:
                                e[0] |= 1 << level
                                e[1] = min(e[1], steps[i])
        dropped = 0
        for (key, _, _), (mask, step, m, kind) in merged.items():
            if not mask & (1 << n['level']):
                dropped += 1    # it stands at other levels only
                continue
            asset_ids.setdefault(key, kind)
            rows.append((n['guid'], key, kind, mask, step, m))
        stats[n['name']] = (n['level'], sum(1 for r in rows if r[0] == n['guid'] and r[2] == 'M'), sum(1 for r in rows if r[0] == n['guid'] and r[2] == 'L'), dropped)
    assets = sorted(asset_ids.items(), key=lambda kv: (kv[1], kv[0]))
    aid = {k: i for i, (k, _) in enumerate(assets)}
    rows.sort(key=lambda r: (r[0], aid[r[1]], r[3], tuple(round(v, 3) for v in r[5])))
    lines = ['# town_layout_items v1: the placed static meshes (M) and level instances (L) of the five towns\' layouts, each node at the level it starts at (tools/capture/gen_town_layouts.py)']
    lines.append('# node levels: ' + ', '.join('%s %d' % (n['name'], n['level']) for n in nodes))
    lines.append('# columns of a row: node, asset, levels (hex, bit n = node level n), stepup (0: a character cannot step up onto it), m00 m01 m02 m10 m11 m12 m20 m21 m22 tx ty tz (world cm)')
    for n in nodes:
        lines.append('@node\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s' % (n['guid'], n['name'], n['layout'], n['type'], num(n['x'], 6), num(n['y'], 6), num(n['z'], 6), num(n['yaw'], 9)))
    for i, (key, kind) in enumerate(assets):
        lines.append('@asset\t%d\t%s\t%s' % (i, kind, key))
    for g, key, kind, mask, step, m in rows:
        lines.append('%d\t%d\t%x\t%d\t%s\t%s' % (g, aid[key], mask, step, '\t'.join(rotnum(v) for v in m[:9]), '\t'.join(num(v, 3) for v in m[9:])))
    return '\n'.join(lines) + '\n', stats, len(assets), len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--content-db', default=DEFAULT_DB)
    ap.add_argument('--seed', default=DEFAULT_SEED, help='the server\'s seed file: the level each bowl starts at (a missing file: level 0 for all)')
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    text, stats, nassets, nrows = build(a.content_db, a.seed)
    data = text.encode('utf-8')
    for name, (level, m, l, dropped) in stats.items():
        print('  %s at level %d: %d static mesh rows, %d level instance rows (%d more stand at other levels only)' % (name, level, m, l, dropped))
    print('gen_town_layouts: %d assets, %d rows, %d bytes' % (nassets, nrows, len(data)))
    if a.check:
        if not os.path.exists(a.out):
            print('gen_town_layouts: %s is missing' % a.out)
            return 1
        have = open(a.out, 'rb').read()
        same = have == data
        print('gen_town_layouts: %s %s' % (a.out, 'identical' if same else 'DIFFERS'))
        return 0 if same else 1
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tmp = a.out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, a.out)
    print('gen_town_layouts: wrote %s' % a.out)
    return 0


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    sys.exit(main())
