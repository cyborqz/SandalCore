# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""An independent Python port of worldd's node populations, the oracle of the golden counts of Nodes 1, stage 3b.

The C++ is server/worldd/spawns/node_population.{h,cpp} (select_node_spawns) over spawns/population.cpp's point rules and spawns/spawn_filter.cpp; this file does the same from
content.db with sqlite and plain Python (pop_oracle.py's data and filter), so that the two can disagree.

What a node's people are (data_world.md s3.4, s3.5; the frames were fitted to the retail captures: capture 721's 506 town NPCs, 407 of them within 3 m of a layout point placed at the
node RECORD's location and yaw, and 81 of the rest within 3 m of a building point placed at the plot's transform in that frame; the other readings of the plot's yaw hit 7 and 25):

    layout sets     `node_layout_population`, the node's layout's PopulationSetsIds: each set is placed at the node's location with the node's yaw (placement id = the node's guid)
    building sets   for each plot of the layout at the node's level (the node wire's plot rows: world/node_wire.cpp) with a building and a state of 1 (UnderConstruction) or 2
                    (Operational): the building's BuildingLevelSpawn of the node's level (`building_pop_spawn`, the highest level key at or below the node's) and its sets of that state
                    (`building_level_spawn_pop`), each placed at the plot's transform in the node's frame (placement id = node ^ plot * 0x9E3779B97F4A7C15)
    the context     the node's id, type, level, biome and zone of influence, Kaelar, Summer, noon, the SiegeInactive tag; the spawn sources Layout and BuildingLevelSpawn are running; the
                    siege tags a filter requires are any-of; the humanoids whose body needs an NPCAppearanceComponent pass when a retail capture opened that Blueprint (its open carries one)

It needs content.db (mode=ro), npc_motion.tsv and spawns/area_spawns.h (pop_oracle.Data); nothing is written.  Used by gen_node_pop_golden.py, which writes
server/worldd/tests/node_pop_golden.h.
"""
import collections
import copy
import math
import os
import re

import pop_oracle as P

M64 = P.M64
LEVELS = P.LEVELS
NODE_TYPES = ['Default', 'Scientific', 'Divine', 'Martial', 'Economic']
BIOMES = ["Default", "AlpineForest", "AncientForest", "ArcticTundra", "Asian", "Badlands", "Desert", "Fjords", "Glacier", "Grasslands", "Iceland", "Jungle", "MagicalForest",
          "RedwoodForest", "Riverlands", "Swamp", "Taiga", "TabletopMountains", "Tropics", "Volcano", "UnderRealm", "Highlands"]
GATES = {'Layout', 'BuildingLevelSpawn'}
PHI = 0x9E3779B97F4A7C15


class NodeData:
    """The node tables of content.db: the nodes, the layouts' population sets and plots, the buildings' populations."""
# Cyb.C-O-A

    def __init__(self, db):
        Q = lambda s, *a: db.execute(s, a).fetchall()
        self.zones = {r[0]: dict(name=r[1], biome=r[2], node=r[3]) for r in Q("SELECT guid,name,biome_ord,node_guid FROM city_zoi")}
        self.nodes = {}
        for r in Q("SELECT rank,guid,name,node_name,node_type_ord,map_name,x,y,z,yaw,layout_guid,zoi_guid,levelable FROM city_node ORDER BY rank"):
            self.nodes[r[1]] = dict(rank=r[0], guid=r[1], name=r[2], display=r[3], type=r[4], map=r[5], x=r[6], y=r[7], z=r[8], yaw=r[9], layout=r[10], zoi=r[11], levelable=bool(r[12]))
        self.layout_sets = collections.defaultdict(list)
        for r in Q("SELECT layout_guid,population_set_guid FROM node_layout_population ORDER BY layout_guid,ord"):
            self.layout_sets[r[0]].append(r[1])
        self.plots = collections.defaultdict(list)
        sql = ("SELECT p.layout_guid,p.level,p.plot_guid,COALESCE(sp.pre_construct,0),COALESCE(sp.empty_building_guid,0),"
               "(SELECT COUNT(*) FROM service_plot_building b WHERE b.plot_guid=p.plot_guid),"
               "COALESCE((SELECT b.building_guid FROM service_plot_building b WHERE b.plot_guid=p.plot_guid ORDER BY b.ord LIMIT 1),0),"
               "p.qx,p.qy,p.qz,p.qw,p.tx,p.ty,p.tz,COALESCE(sp.starting_state,0),COALESCE(sp.name,'') "
               "FROM node_layout_plot p LEFT JOIN service_plot sp ON sp.guid=p.plot_guid ORDER BY p.layout_guid,p.level,p.idx")
        for r in Q(sql):
            building = r[6] if r[5] == 1 else r[4]
            if building == 0 and r[5] > 0:
                building = r[6]
            large = 'ConstructionProject_Large' in (r[15] or '')
            state = 0 if not r[3] else (r[14] if large else 2)
            self.plots[(r[0], r[1])].append(dict(plot=r[2], building=building, state=state, q=(r[7], r[8], r[9], r[10]), t=(r[11], r[12], r[13])))
        self.keys = collections.defaultdict(dict)    # building -> {level: spawn guid}
        for r in Q("SELECT building_guid,level,spawn_guid FROM building_pop_spawn"):
            self.keys[r[0]][r[1]] = r[2]
        self.spawn_sets = collections.defaultdict(list)    # (spawn guid, state) -> set guids
        for r in Q("SELECT spawn_guid,state,population_set_guid FROM building_level_spawn_pop ORDER BY spawn_guid,state,ord"):
            self.spawn_sets[(r[0], r[1])].append(r[2])

    def spawn_for(self, building, level):
        """The BuildingLevelSpawn a building runs at a node level: the highest key at or below the level (0: none)."""
        best = None
        for k in self.keys.get(building, {}):
            if k <= level and (best is None or k > best):
                best = k
        return self.keys[building][best] if best is not None else 0


def quat_yaw(qx, qy, qz, qw):
    return math.degrees(math.atan2(2.0 * (qw * qz + qx * qy), 1.0 - 2.0 * (qy * qy + qz * qz)))


def node_context(base, node, level, zones):
    ctx = copy.copy(base)
    ctx.node_level = LEVELS[level]
    ctx.node_id = node['guid']
    ctx.node_type = NODE_TYPES[node['type']]
    z = zones.get(node['zoi'])
    ctx.biome = BIOMES[z['biome']] if z else ''
    ctx.zone = node['zoi']
    ctx.active_gates = set(GATES)
    return ctx


def sources(nd, node, level):
    """[(placement id, set guid, x, y, z, yaw, kind, plot)] of the node's population sets at a level, in order: the layout's, then each plot's."""
    out = []
    for sg in nd.layout_sets.get(node['layout'], ()):
        out.append((node['guid'], sg, node['x'], node['y'], node['z'], node['yaw'], 'layout', 0))
    ny = node['yaw'] * math.pi / 180.0
    for p in nd.plots.get((node['layout'], level), ()):
        if p['building'] == 0 or p['state'] not in (1, 2):
            continue
        spawn = nd.spawn_for(p['building'], level)
        if not spawn:
            continue
        tx, ty, tz = p['t']
        x = node['x'] + tx * math.cos(ny) - ty * math.sin(ny)
        y = node['y'] + tx * math.sin(ny) + ty * math.cos(ny)
        yaw = node['yaw'] + quat_yaw(*p['q'])
        placement = (node['guid'] ^ ((p['plot'] * PHI) & M64)) & M64
        for sg in nd.spawn_sets.get((spawn, p['state']), ()):
            out.append((placement, sg, x, y, node['z'] + tz, yaw, 'building', p['plot']))
    return out


def select_node(d, nd, node, level, base_ctx, contains, shown_extra=()):
    """The node's spawned NPCs at a level: (stage counts, filtered by reason, NPCs by Blueprint, [(x, y, level, leaf, kind, plot, set, key)]).  `d` is pop_oracle.Data, `contains(x, y)`
    the area's footprint (a point outside it is dropped: the ground ends there)."""
    ctx = node_context(base_ctx, node, level, nd.zones)
    ctx.shown = set(ctx.shown) | set(shown_extra)
    st = collections.Counter()
    filt = collections.Counter()
    byarch = collections.Counter()
    spawned = []
    for (placement, sg, ox, oy, oz, oyaw, kind, plot) in sources(nd, node, level):
        s = d.sets.get(sg)
        if not s:
            st['no_set'] += 1
            continue
        pl = dict(spawn=placement, key=0, set=sg, x=ox, y=oy, z=oz, yaw=oyaw, minl=0, maxl=0)
        if s['area']:
            ar = d.areas.get(s['area'])
            if not ar:
                continue
            pts = P.area_points(pl, s, ar)
        else:
            pts = P.placement_points(pl, s)
        st['sets_' + kind] += 1
        for p in pts:
            st['points_' + kind] += 1
            if not contains(p['x'], p['y']):
                st['outside'] += 1
                continue
            inst = d.inst.get(p['inst'])
            if not inst:
                continue
            w = P.f_point(d, None, s, inst, ctx)
            if w != 'Spawns':
                filt[w] += 1
                continue
            zoi = 0
            allc, shown = [], []
            for asg in inst['asset_sets']:
                if asg not in d.asset_sets:
                    continue
                for ag, wgt in d.asset_sets[asg]:
                    a = d.assets.get(ag)
                    if not a or not a['path']:
                        continue
                    w = P.f_asset(d, None, s, inst, asg, a, ctx, zoi)
                    if w != 'Spawns':
                        allc.append((a, wgt if wgt > 0 else 1.0, False, w))
                        continue
                    ok = bool(a['path']) and (a['path'] in d.captured_npc or a['path'] in d.motion)
                    allc.append((a, wgt if wgt > 0 else 1.0, ok, 'Spawns'))
                    if ok:
                        shown.append((a, wgt if wgt > 0 else 1.0))
            h = P.point_hash(inst['guid'], s['guid'], placement, p['key'])
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
            r = P.unit(P.mix(h ^ 0xA55E7)) * total
            chosen = shown[-1]
            for c in shown:
                if r < c[1]:
                    chosen = c
                    break
                r -= c[1]
            lo, hi = inst['lmin'], inst['lmax']
            if hi < lo:
                hi = lo
            lev = lo + (P.mix(h ^ 0x1E7E1) % (hi - lo + 1)) if lo > 0 else 0
            lf = P.leaf(chosen[0]['path'])
            st['spawned'] += 1
            st['spawned_' + kind] += 1
            byarch[lf] += 1
            spawned.append((p['x'], p['y'], lev, lf, kind, plot, sg, p['key']))
    return st, filt, byarch, spawned
# -- C.O.A . Cyb --


def digest(spawned):
    """An order-free hash of the spawned NPCs (x, y in cm, level, Blueprint): pop_oracle.digest over the first four fields."""
    return P.digest([(x, y, lev, lf) for (x, y, lev, lf, *_rest) in spawned])


def captured_leaves(d):
    """The Blueprint leaves of the NPCs a retail capture opened (their opens carry the appearance component)."""
    return {P.leaf(p) for p in d.captured_npc}
