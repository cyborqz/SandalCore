# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""server/worldd/spawns/garden_defs.h: the GatherableActorDefs of the GM garden (G1, stage 1, Part B).

The GM garden is one resource node for each gathering profession (Mining, Lumberjacking, Herbalism) and certification tier (Novice, Apprentice, Journeyman,
Master, GrandMaster), near the login spot, behind worldd's `--gm-garden` / AOC_GM_GARDEN (off by default).  Its nodes are real GatherableActorDefs of the design
data, found through the data's own chain:

    GatherableActorDefRecordBase.FoliageActorDefId -> GatheringFoliageDataRecordBase.GatheringResourceTypeId -> GatheringResourceTypeRecordBase
        (RequiredProfessionIds, RequiredCertificationLevelId)

and cross-checked with the interactions the def's foliage states name (GatheringFoliageStateRecordBase.FoliageInteractionsIds -> GatheringInteractionDataRecordBase:
ProfessionId, CertificationLevelId, ToolType, BaseGatheringDuration), which are what the server will test in stage 2.

What the data has (content.db version 11): actor nodes at Novice, Apprentice and Journeyman for all three professions, and at GrandMaster for Herbalism only (the
design record "TestSpiritBloom": the one GrandMaster actor node, a designers' test def).  There is NO GatherableActorDef at Master for any profession, and none at
GrandMaster for Mining or Lumberjacking: the Master and GrandMaster interactions of Mining (silver, gneiss, opal; quartz, gold, diamond) belong to painted foliage (stage 3),
which is no actor.  So 10 of the 15 slots have a def; the header lists the 5 that have not (worldd logs them at start and places nothing there).

The tier of a def is what its harvest asks for: the highest certification among the interactions of its foliage states.  The pick inside a (profession, tier) slot,
deterministic: the Riverlands' own flora named in PREFER first; then not dev only, not a placeable (Freehold), a quest or event copy (FTUE, TEST, NodeWar, Corrupted,
Livestock, RELIC) unless nothing else fills the slot; then a Large variant before a Medium one, variant 1 before the others, then the name.

It reads dist/content/content.db (--content-db), mode=ro, and nothing else:

    python tools/capture/gen_gm_garden.py [--content-db PATH]          # rewrite the header
    python tools/capture/gen_gm_garden.py [--content-db PATH] --check  # write nothing, exit 1 on a change
"""
import argparse
import gzip
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
HEADER = os.path.join(ROOT, 'server', 'worldd', 'spawns', 'garden_defs.h')
DEFAULT_DB = os.path.join(ROOT, 'dist', 'content', 'content.db')

PROFESSIONS = ['Mining', 'Lumberjacking', 'Herbalism']
TIERS = ['Novice', 'Apprentice', 'Journeyman', 'Master', 'GrandMaster']
AVOID = ('Freehold', 'FTUE', 'TEST', 'Test', 'NodeWar', 'Corrupted', 'Livestock', 'RELIC', 'ZDELETE')


def load(db, schema):
    out = {}
    for row in db.execute('SELECT guid, type_id, name, dev_only, data FROM record WHERE schema = ?', (schema,)):
        out[row[0]] = {'guid': row[0], 'type_id': row[1], 'name': row[2], 'dev_only': bool(row[3]), 'data': json.loads(gzip.decompress(row[4]))}
    return out


def collect(db_path):
    db = sqlite3.connect('file:' + db_path.replace('\\', '/') + '?mode=ro', uri=True)
    defs = load(db, 'GatherableActorDefRecordBase')
    foliage = load(db, 'GatheringFoliageDataRecordBase')
    rtypes = load(db, 'GatheringResourceTypeRecordBase')
    certs = load(db, 'CertificationLevelRecordBase')
    profs = load(db, 'CharacterProfessionsRecordBase')
    states = load(db, 'GatheringFoliageStateRecordBase')
    inter = load(db, 'GatheringInteractionDataRecordBase')
    meta = dict(db.execute('SELECT key, value FROM meta').fetchall())
    cands = {}
    for g, d in defs.items():
        f = foliage.get(d['data']['FoliageActorDefId']['Guid'])
        if not f:
            continue
        t = rtypes.get(f['data']['GatheringResourceTypeId']['Guid'])
        if not t:
            continue
        ps = [profs[p['Guid']]['name'] for p in t['data']['RequiredProfessionIds'] if p['Guid'] in profs]
        c = certs.get(t['data']['RequiredCertificationLevelId']['Guid'])
        if len(ps) != 1 or ps[0] not in PROFESSIONS or not c or c['name'] not in TIERS:
            continue
        # the interactions of the def's foliage states: what the server will check in stage 2
        seen = set()
        for kv in f['data']['FoliageStateDataIds']:
            s = states.get(kv['Value']['Guid'])
            for ii in (s['data'].get('FoliageInteractionsIds', []) if s else []):
                i = inter.get(ii['Guid'])
                if i:
                    ip = profs.get(i['data']['ProfessionId']['Guid'])
                    ic = certs.get(i['data']['CertificationLevelId']['Guid'])
                    seen.add((ip['name'] if ip else '?', ic['name'] if ic else '?', i['data']['ToolType'].split('::')[-1], i['data']['BaseGatheringDuration']))
        # the tier is what the harvest asks for: the highest certification of the interactions of the def's states (the resource type's own requirement
        # disagrees for Plant_BloodboiledSpiritBloom_Medium_1: GrandMaster on the type, Journeyman on the interaction)
        mine = [i for i in seen if i[0] == ps[0] and i[1] in TIERS]
        tier = max((i[1] for i in mine), key=TIERS.index) if mine else c['name']
        out = {'guid': g, 'type_id': d['type_id'], 'name': d['name'], 'dev_only': d['dev_only'], 'profession': ps[0], 'tier': tier, 'interactions': sorted(seen),
               'resource': t['name'], 'interaction': d['data']['InteractionDefId']['Guid'], 'respawn_override': d['data']['OverrideRespawnDuration'], 'empty': f['data']['EmptyDuration']}
        cands[g] = out
    return cands, meta
# Cyb x C.O.A


# The Riverlands' own flora first (the nodes retail's captures saw there: Oak, Ash, Granite, Basalt, Daffodil, Snowdrop, Flax; the Apprentice and Journeyman nodes
# the captures harvested: Weeping Willow, Moonbell, Slate, Limestone), so that the garden looks like the country around it; then the rule in the docstring.
PREFER = {
    ('Mining', 'Novice'): ['Stone_Granite_Large_1', 'Stone_Basalt_Large_1'],
    ('Mining', 'Apprentice'): ['Stone_Slate_Large_1'],
    ('Mining', 'Journeyman'): ['Stone_Limestone_Large_1'],
    ('Lumberjacking', 'Novice'): ['Tree_Oak_Large_1', 'Tree_Ash_Large_1'],
    ('Lumberjacking', 'Apprentice'): ['Tree_WeepingWillow_Large_1'],
    ('Herbalism', 'Novice'): ['Plant_Daffodil_Medium_1', 'Plant_Snowdrop_Medium_1'],
    ('Herbalism', 'Apprentice'): ['Plant_Moonbell_Medium_1'],
    ('Herbalism', 'Journeyman'): ['Plant_Spindlevine_Medium_1'],
}


def rank(c):
    n = c['name']
    pref = PREFER.get((c['profession'], c['tier']), [])
    return (pref.index(n) if n in pref else len(pref), any(a in n for a in AVOID), 0 if '_Large_' in n else 1 if '_Medium_' in n else 2, 0 if n.endswith('_1') else 1, n)


def pick(cands):
    slots = []
    for p in PROFESSIONS:
        for ti, t in enumerate(TIERS):
            here = [c for c in cands.values() if c['profession'] == p and c['tier'] == t and not c['dev_only']]
            here.sort(key=rank)
            slots.append((p, ti, t, here[0] if here else None, len(here)))
    return slots


def render(slots, meta):
    out = ['''// ============================================================================
//  server/worldd/spawns/garden_defs.h
//
//  GENERATED by tools/capture/gen_gm_garden.py from content.db (contentc version %s); do not edit.
//
//  The GatherableActorDefs of the GM garden (spawns/gm_garden.h): one real def for each gathering profession and certification tier the design data has an actor node
//  for.  The data has none at Master, and none at GrandMaster for Mining and Lumberjacking (those tiers' nodes are painted foliage, no actor): kGardenGaps lists them.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace aoc::worldd::spawns::garden {

/// A GatherableActorDef record: its Guid and TypeId as the actor's replicated GatherableActorDef struct names them.
struct Def {
    const char* profession;     // Mining, Lumberjacking, Herbalism
    int         tier;           // 0 Novice .. 4 GrandMaster
    const char* tier_name;
    uint64_t    guid;
    uint64_t    type_id;
    uint64_t    interaction;    // its PlayerInteractionDef record (the node's InteractableComponent names it)
    const char* name;           // the record's name
    const char* resource;       // its GatheringResourceType ("Tree_Oak")
    const char* tool;           // the interaction's tool slot (Axe, Pickaxe, Sickle)
    double      base_duration;  // the interaction's BaseGatheringDuration, seconds
    int         candidates;     // the defs the slot had to choose from
};

/// A profession and tier the design data has no actor node for.
struct Gap {
    const char* profession;
    int         tier;
    const char* tier_name;
};
''' % meta.get('contentc.version', '?')]
    out.append('inline constexpr Def kGardenDefs[] = {')
    gaps = []
    for p, ti, t, c, n in slots:
        if c is None:
            gaps.append((p, ti, t))
            continue
        inter = [i for i in c['interactions'] if i[0] == p and i[1] == t]
        inter.sort(key=lambda i: -i[3])    # the longest (the Large variant's)
        tool = inter[0][2] if inter else '?'
        dur = inter[0][3] if inter else 0
        out.append('    {"%s", %d, "%s", %dull, %dull, %dull, "%s", "%s", "%s", %r, %d},' % (p, ti, t, c['guid'], c['type_id'], c['interaction'], c['name'], c['resource'], tool, float(dur), n))
    out.append('};')
    out.append('inline constexpr size_t kGardenDefCount = sizeof(kGardenDefs) / sizeof(kGardenDefs[0]);\n')
    out.append('inline constexpr Gap kGardenGaps[] = {')
    for p, ti, t in gaps:
        out.append('    {"%s", %d, "%s"},' % (p, ti, t))
    out.append('};')
    out.append('inline constexpr size_t kGardenGapCount = sizeof(kGardenGaps) / sizeof(kGardenGaps[0]);\n')
    out.append('}  // namespace aoc::worldd::spawns::garden\n')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--content-db', default=os.environ.get('AOC_CONTENT_DB') or DEFAULT_DB)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    if not os.path.exists(a.content_db):
        sys.exit('content.db not found: %s' % a.content_db)
    cands, meta = collect(a.content_db)
    slots = pick(cands)
    for p, ti, t, c, n in slots:
        print('%-14s %-11s %s' % (p, t, ('%s (%d candidates)' % (c['name'], n)) if c else 'NO ACTOR DEF'))
    text = render(slots, meta)
    old = open(HEADER, newline='').read() if os.path.exists(HEADER) else None
    same = old is not None and old.replace('\r\n', '\n') == text
    if a.check:
        print('garden_defs.h: ' + ('unchanged' if same else 'WOULD CHANGE'))
        sys.exit(0 if same else 1)
    if same:
        print('garden_defs.h: unchanged')
        return
    crlf = old is not None and '\r\n' in old
    with open(HEADER, 'w', newline='') as f:
        f.write(text.replace('\n', '\r\n') if crlf else text)
    print('garden_defs.h: updated')


if __name__ == '__main__':
    main()
