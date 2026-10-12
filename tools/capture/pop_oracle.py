# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""An independent Python port of worldd's population selection, the oracle of the region's golden counts (G1, stage 1, Part B).

The C++ is server/worldd/spawns/population.cpp (select_data_spawns, area_spawn_points, placement_points) and spawns/spawn_filter.cpp; this file does the same from
content.db with sqlite and plain Python, so that the two can disagree.  It began as the research's `pop.py` (research/g1region), which reproduced E1's numbers exactly (1,803
NPCs for the 1 km circle), and has two additions for the Riverlands:

    * the footprint (fixtures/region/riverlands_rects.json) and the zone of influence of a point (riverlands_zoi_rects.json) come from the JSON the generators read,
      not from the header the C++ reads: a bug in region_data.h or in the C++ index shows;
    * the filter's zone rule: a PopulationFilter that names zones of influence passes when the point's ZOI is one of them (without ZOIs, `zoi=False`, such a filter
      never passes: the research's pop.py and E1).

It needs content.db (mode=ro), npc_motion.tsv and spawns/area_spawns.h (the archetypes a retail capture opened as NPC templates); nothing is written.
Used by gen_pop_golden.py, which writes server/worldd/tests/pop_golden_region.h.
"""
import bisect
import collections
import json
import math
import os
import re
import sqlite3

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
M64 = (1 << 64) - 1
LEVELS = ["Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis", "cCount"]


def mix(z):
    z = (z + 0x9E3779B97F4A7C15) & M64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


def u64(v):
    return v & M64


def point_hash(instance, set_, placement, key):
    h = mix(u64(instance))
    h = mix(h ^ u64(set_))
    h = mix(h ^ u64(placement))
    h = mix(h ^ u64(key))
    return h


def unit(h):
    return (h >> 11) / float(1 << 53)


def lvl(n):
    return LEVELS.index(n) if n in LEVELS else -1


def leaf(path):
    i = path.rfind('.')
    l = path[i + 1:] if i >= 0 else path
    if l.startswith('Default__'):
        l = l[9:]
    if l.endswith('_C') and len(l) > 2:
        l = l[:-2]
    return l
# C.Y.B + C-O-A


class Region:
    """The footprint and the ZOI of a point, from the JSON the generators read (rows of 8,192 cm for the lookups)."""

    def __init__(self, root=ROOT):
        r = json.load(open(os.path.join(root, 'fixtures', 'region', 'riverlands_rects.json')))
        z = json.load(open(os.path.join(root, 'fixtures', 'region', 'riverlands_zoi_rects.json')))
        self.rects = [tuple(int(v) for v in q) for q in r['rects_xy_cm']]
        self.zois = [(int(q['guid']), q['name'], [tuple(int(v) for v in a) for a in q['rects_xy_cm']]) for q in z['zois']]
        self.rows = {}
        for i, (x0, y0, x1, y1) in enumerate(self.rects):
            for k in range(y0 // 8192, (y1 - 1) // 8192 + 1):
                self.rows.setdefault(k, []).append(i)
        self.zrows = {}
        for zi, (guid, name, rs) in enumerate(self.zois):
            for (x0, y0, x1, y1) in rs:
                for k in range(y0 // 8192, (y1 - 1) // 8192 + 1):
                    self.zrows.setdefault(k, []).append((x0, y0, x1, y1, guid))

    def contains(self, x, y):
        if not (math.isfinite(x) and math.isfinite(y)):
            return False
        ix, iy = math.floor(x), math.floor(y)
        for i in self.rows.get(iy // 8192, ()):
            x0, y0, x1, y1 = self.rects[i]
            if x0 <= ix < x1 and y0 <= iy < y1:
                return True
        return False

    def zoi(self, x, y):
        ix, iy = math.floor(x), math.floor(y)
        for (x0, y0, x1, y1, guid) in self.zrows.get(iy // 8192, ()):
            if x0 <= ix < x1 and y0 <= iy < y1:
                return guid
        return 0


class Data:
    def __init__(self, content_db, motion_tsv, spawns_header):
        self.db = sqlite3.connect('file:' + content_db.replace('\\', '/') + '?mode=ro', uri=True)
        Q = lambda s: self.db.execute(s).fetchall()
        self.assets = {}
        for r in Q("SELECT guid,name,archetype_path,dev_only,filter,tags FROM npc_asset"):
            self.assets[r[0]] = dict(guid=r[0], name=r[1], path=r[2], dev=bool(r[3]), filter=json.loads(r[4]), tags=json.loads(r[5]) if r[5] else [])
        self.asset_sets = collections.defaultdict(list)
        self.asset_set_tags = {}
        for r in Q("SELECT guid,tags FROM npc_asset_set"):
            self.asset_sets[r[0]]
            t = json.loads(r[1]) if r[1] else []
            if t:
                self.asset_set_tags[r[0]] = t
        for r in Q("SELECT set_guid,asset_guid,weight FROM npc_asset_set_member ORDER BY set_guid,ord"):
            self.asset_sets[r[0]].append((r[1], r[2]))
        self.inst = {}
        for r in Q("SELECT guid,name,level_min,level_max,blacklisted,dev_only,filter_override_guid,tags,vendor_guid,ambient_behavior,respawn_time FROM npc_instance"):
            self.inst[r[0]] = dict(guid=r[0], name=r[1], lmin=r[2], lmax=r[3], black=bool(r[4]), dev=bool(r[5]), fo=r[6], tags=json.loads(r[7]) if r[7] else [],
                                   vendor=r[8], beh=r[9], resp=r[10], asset_sets=[])
        for r in Q("SELECT instance_guid,set_guid FROM npc_instance_asset_set ORDER BY instance_guid,ord"):
            if r[0] in self.inst:
                self.inst[r[0]]['asset_sets'].append(r[1])
        self.sets = {}
        for r in Q("SELECT guid,name,area_guid,blacklisted,dev_only,filter_override_guid,tags FROM npc_set"):
            self.sets[r[0]] = dict(guid=r[0], name=r[1], area=r[2], black=bool(r[3]), dev=bool(r[4]), fo=r[5], tags=json.loads(r[6]) if r[6] else [], members=[])
        for r in Q("SELECT set_guid,key,instance_guid,x,y,z,yaw FROM npc_set_member ORDER BY set_guid,key"):
            if r[0] in self.sets:
                self.sets[r[0]]['members'].append(dict(key=r[1], inst=r[2], x=r[3], y=r[4], z=r[5], yaw=r[6]))
        self.ws = {}
        for r in Q("SELECT guid,name,culture,map_name,dev_only,filter_override_guid FROM npc_world_spawn"):
            self.ws[r[0]] = dict(guid=r[0], name=r[1], map=r[3], dev=bool(r[4]), fo=r[5])
        self.filters = {r[0]: json.loads(r[1]) for r in Q("SELECT guid,filter FROM npc_filter")}
        self.gates = collections.defaultdict(list)
        for r in Q("SELECT set_guid,source FROM npc_gate ORDER BY set_guid,source"):
            self.gates[r[0]].append(r[1])
        self.placements = [dict(spawn=r[0], key=r[1], set=r[2], x=r[3], y=r[4], z=r[5], yaw=r[6], minl=r[7], maxl=r[8])
                           for r in Q("SELECT spawn_guid,key,set_guid,x,y,z,yaw,min_level,max_level FROM npc_placement ORDER BY spawn_guid,key")]
        self.areas = {}
        for r in Q("SELECT guid,name,min_spawns,max_spawns FROM npc_area"):
            self.areas[r[0]] = dict(guid=r[0], lo=r[2], hi=r[3], circles=[])
        for r in Q("SELECT area_guid,x,y,z,extent,weight,max_spawns FROM npc_area_circle ORDER BY area_guid,ord"):
            if r[0] in self.areas:
                self.areas[r[0]]['circles'].append(dict(x=r[1], y=r[2], z=r[3], ext=r[4], w=r[5], mx=r[6]))
        self.motion = {}
        with open(motion_tsv, encoding='utf-8') as f:
            f.readline()
            for line in f:
                c = line.rstrip('\n').split('\t')
                if len(c) < 17:
                    continue
                self.motion[c[0]] = dict(mesh=c[13], anim=c[14], placeholder=c[15] == '1', hidden=c[16] == '1')
        src = open(spawns_header, encoding='utf-8').read()
        self.captured_npc = set(re.findall(r'\{"npc", "ANPCCharacter", "([^"]+)"', src))


class Context:
    def __init__(self, hour=11.0):
        self.map = 'Verra_World_Master'
        self.node_level = 'Wilderness'
        self.culture = 'Kaelar'
        self.biome = ''
        self.season = 'Summer'
        self.weather = ''
        self.node_type = ''
        self.node_id = 0
        self.hour = hour
        self.world_tags = {'War.Siege.States.SiegeInactive'}
        self.shown = {'Guard_Soldier'}
        self.humanoids = False
        self.enabled = True
        self.zone = 0                      # Nodes 1, stage 3b: the context's own zone of influence, used when the point has none (a node's)
        self.active_gates = set()          # Nodes 1, stage 3b: the spawn sources that are running (a node's populations: Layout, BuildingLevelSpawn)
# [C.O.A - Cyb]


def f_point(d, ws, s, i, ctx):
    if (ws and ws['dev']) or s['dev'] or i['dev']:
        return 'DevOnly'
    if not ctx.enabled:
        return 'Blacklisted' if (s['black'] or i['black']) else 'Spawns'
    if ws and ws['map'] != ctx.map:
        return 'NotWorldMap'
    if s['black'] or i['black']:
        return 'Blacklisted'
    for src in d.gates.get(s['guid'], ()):
        if src not in ctx.active_gates:
            return 'Gated'
    return 'Spawns'


def f_filter(f, tags, ctx, zoi):
    level = lvl(ctx.node_level)
    lo = lvl(f['node_level_min'])
    hi = lvl(f['node_level_max'])
    if lo < 0 or hi < 0 or lo >= 7 or hi >= 7 or level < lo or level > hi:
        return 'NodeLevel'
    if f['cultures'] and ctx.culture not in f['cultures']:
        return 'Culture'
    if f['node_ids'] and ctx.node_id not in f['node_ids']:
        return 'NodeOrZone'
    zone = zoi if zoi else ctx.zone
    if f['zones_of_influence'] and (zone == 0 or zone not in f['zones_of_influence']):
        return 'NodeOrZone'
    if f['node_types'] and ctx.node_type not in f['node_types']:
        return 'NodeType'
    if f['biomes'] and ctx.biome not in f['biomes']:
        return 'Biome'
    if f['seasons'] and ctx.season not in f['seasons']:
        return 'Season'
    if f['weather'] and ctx.weather not in f['weather']:
        return 'Weather'
    a = f['time_of_day_start']
    b = f['time_of_day_end']
    if a != -1 or b != -1:
        h = ctx.hour
        inn = (a <= h <= b) if a <= b else (h >= a or h <= b)
        if not inn:
            return 'TimeOfDay'
    # Nodes 1, stage 3b: the siege states are mutually exclusive, so the siege tags a filter requires are ANY-of (a node carries SiegeInactive); every other required tag is all-of
    siege = [t for t in f['required_tags'] if t.startswith('War.Siege.States.')]
    if siege and not any(t in tags or t in ctx.world_tags for t in siege):
        return 'RequiredTag'
    for t in f['required_tags']:
        if t.startswith('War.Siege.States.'):
            continue
        if t not in tags and t not in ctx.world_tags:
            return 'RequiredTag'
    return 'Spawns'


def f_visuals(d, a, ctx):
    if not a['path']:
        return 'Spawns'
    m = d.motion.get(a['path'])
    if m is None:
        return 'Spawns'
    if m['hidden']:
        return 'Hidden'
    if m['placeholder']:
        return 'Placeholder'
    if not m['mesh']:
        return 'NoMesh'
    if not m['anim']:
        return 'NoAnimation'
    if 'SK_PC_MASTER' in m['mesh'] and not ctx.humanoids and leaf(a['path']) not in ctx.shown:
        return 'Appearance'
    return 'Spawns'

# (C-Y-B + CoA)

def f_asset(d, ws, s, i, asset_set, a, ctx, zoi):
    if a['dev']:
        return 'DevOnly'
    if not ctx.enabled:
        return 'Spawns'
    fo = i['fo'] or s['fo'] or (ws['fo'] if ws else 0)
    f = a['filter']
    if fo and fo in d.filters:
        f = d.filters[fo]
    tags = set(i['tags']) | set(s['tags']) | set(a['tags']) | set(d.asset_set_tags.get(asset_set, []))
    w = f_filter(f, tags, ctx, zoi)
    if w != 'Spawns':
        return w
    if 'npc.type.prop' in a['tags']:
        return 'Prop'
    return f_visuals(d, a, ctx)


def placement_points(pl, s):
    yaw = pl['yaw'] * math.pi / 180.0
    c = math.cos(yaw)
    sn = math.sin(yaw)
    return [dict(x=pl['x'] + m['x'] * c - m['y'] * sn, y=pl['y'] + m['x'] * sn + m['y'] * c, z=pl['z'] + m['z'], yaw=pl['yaw'] + m['yaw'], key=m['key'], inst=m['inst'])
            for m in s['members']]


def area_points(pl, s, ar):
    if not s['members'] or not ar['circles']:
        return []
    lo = max(0, ar['lo'])
    hi = max(lo, ar['hi'])
    base = point_hash(s['guid'], ar['guid'], pl['spawn'], pl['key'])
    count = lo + (mix(base ^ 0xC0DE5) % (hi - lo + 1))
    wt = [max(0.0, c['w']) * c['ext'] * c['ext'] for c in ar['circles']]
    used = [0] * len(ar['circles'])

    def room(i):
        return ar['circles'][i]['mx'] < 0 or used[i] < ar['circles'][i]['mx']
    out = []
    for n in range(count):
        h = mix(base ^ (((n + 1) * 0x9E3779B97F4A7C15) & M64))
        total = sum(wt[i] for i in range(len(wt)) if room(i))
        if not total > 0:
            break
        r = unit(mix(h ^ 0x51C)) * total
        pick = len(wt)
        for i in range(len(wt)):
            if not room(i) or wt[i] <= 0:
                continue
            pick = i
            if r < wt[i]:
                break
            r -= wt[i]
        if pick >= len(wt):
            break
        used[pick] += 1
        c = ar['circles'][pick]
        rad = c['ext'] * math.sqrt(unit(mix(h ^ 0xA1)))
        ang = unit(mix(h ^ 0xA2)) * 2.0 * math.pi
        m = s['members'][n % len(s['members'])]
        out.append(dict(x=pl['x'] + c['x'] + rad * math.cos(ang), y=pl['y'] + c['y'] + rad * math.sin(ang), z=pl['z'] + c['z'], yaw=unit(mix(h ^ 0xA3)) * 360.0,
                        key=0x10000 + n, inst=m['inst']))
    return out


def select(d, contains, zoi_at, ctx):
    """The region's selection: counts by name, the spawned NPCs, the points lost by reason.  `zoi_at(x, y)` is the point's zone (0: none)."""
    st = collections.Counter()
    filt = collections.Counter()
    byarch = collections.Counter()
    byzoi = collections.Counter()
    spawned = []
    for pl in d.placements:
        st['placements'] += 1
        s = d.sets.get(pl['set'])
        if not s:
            continue
        fromarea = False
        if s['area']:
            ar = d.areas.get(s['area'])
            if not ar:
                continue
            pts = area_points(pl, s, ar)
            fromarea = True
            if any(contains(p['x'], p['y']) for p in pts):
                st['area_sets'] += 1
        else:
            pts = placement_points(pl, s)
        for p in pts:
            if not contains(p['x'], p['y']):
                continue
            st['area_points' if fromarea else 'points_in_area'] += 1
            inst = d.inst.get(p['inst'])
            if not inst:
                continue
            ws = d.ws.get(pl['spawn'])
            w = f_point(d, ws, s, inst, ctx)
            if w != 'Spawns':
                filt[w] += 1
                continue
            zoi = zoi_at(p['x'], p['y'])
            allc = []
            shown = []
            for sg in inst['asset_sets']:
                if sg not in d.asset_sets:
                    continue
                for ag, wgt in d.asset_sets[sg]:
                    a = d.assets.get(ag)
                    if not a or not a['path']:
                        continue
                    w = f_asset(d, ws, s, inst, sg, a, ctx, zoi)
                    if w != 'Spawns':
                        allc.append((a, wgt if wgt > 0 else 1.0, False, w))
                        continue
                    ok = bool(a['path']) and (a['path'] in d.captured_npc or a['path'] in d.motion)
                    allc.append((a, wgt if wgt > 0 else 1.0, ok, 'Spawns'))
                    if ok:
                        shown.append((a, wgt if wgt > 0 else 1.0))
            h = point_hash(inst['guid'], s['guid'], pl['spawn'], p['key'])
            if not allc:
                st['no_asset'] += 1
                continue
            if not shown:
                if all(c[3] != 'Spawns' for c in allc):
                    filt[allc[0][3]] += 1
                    continue
                st['no_template'] += 1
                continue
            total = sum(c[1] for c in shown)
            r = unit(mix(h ^ 0xA55E7)) * total
            chosen = shown[-1]
            for c in shown:
                if r < c[1]:
                    chosen = c
                    break
                r -= c[1]
            lo = inst['lmin']
            hi = inst['lmax']
            if pl['maxl'] > 0 or pl['minl'] > 0:
                lo = pl['minl'] if pl['minl'] > 0 else lo
                hi = pl['maxl'] if pl['maxl'] > 0 else hi
            if hi < lo:
                hi = lo
            lev = lo + (mix(h ^ 0x1E7E1) % (hi - lo + 1)) if lo > 0 else 0
            st['spawned'] += 1
            if fromarea:
                st['area_spawned'] += 1
            lf = leaf(chosen[0]['path'])
            byarch[lf] += 1
            if zoi:
                byzoi[zoi] += 1
            spawned.append((p['x'], p['y'], lev, lf))
    return st, filt, byarch, byzoi, spawned
# Cyb - C-O-A


def digest(spawned):
    """An order-free hash of the spawned NPCs: the multiset of (x, y in cm, level, Blueprint), each hashed (FNV-1a 64) and the hashes summed."""
    total = 0
    for x, y, lev, lf in spawned:
        s = '%d,%d,%d,%s' % (round(x), round(y), lev, lf)
        h = 0xCBF29CE484222325
        for b in s.encode():
            h = ((h ^ b) * 0x100000001B3) & M64
        total = (total + h) & M64
    return total
