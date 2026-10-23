# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Writes npc_targeting.tsv: the targeting record of every ability that npc_abilities.tsv lists.

A targeting record says where an ability's hit lands.  It has a target part (how the target is found, and whether anything is measured at all) and volumes (shapes swept at the hit):
the target part measures its distance, angle and line of sight only when the record's validity check is on; a record with the check off hits the target wherever it stands, whatever distance
and angle it states.  The design database (content.db) keeps the records; this reads them for the abilities the creature tables name.

    python targeting.py --content-db dist/content/content.db --abilities dist/content/npc_abilities.tsv --out dist/content/npc_targeting.tsv

One line per (ability, key), sorted:
  ability, key, distance, min_distance, angle, angle_center, cone_check (1 or 0: the distance and the angle count when the validity check is on),
  freeze (none, pause or stop), freeze_distance (cm), freeze_scale (the creature's own radius counts this many times), ignore_non_target (1: only the ability's target counts)
    (the four freeze values say what the ability's root motion does when a body comes ahead of the creature, the same on each line of an ability),
  discovery (none, prefers_active, only_locked, only_active or prefers_locked: how the target part finds its target),
  check_validity (1 or 0), los_check (1 or 0: the line of sight counts when the validity check is on), distance_center (cm: the cone starts this far ahead of the creature),
  volumes (how many shapes the record has) and shapes (one entry per volume, ';' between them):
    sphere:ox:oy:oz:radius:los                 centred ox ahead of the creature, oy to its right and oz above its feet (cm)
    box:ox:oy:oz:hx:hy:hz:yaw:los              the same, with the half sizes along, across and up, turned yaw degrees from the creature's facing
    other:why                                  a volume that cannot be placed (cone, capsule, point, octree, hollow, moving, origin, tilted ...): the ability keeps the creature's single swing
    (los: 1 when the target has to be seen from the creature.)
"""
import argparse
import gzip
import json
import sqlite3
import sys

HEADER = ('ability\tkey\tdistance\tmin_distance\tangle\tangle_center\tcone_check\tfreeze\tfreeze_distance\tfreeze_scale\tignore_non_target\tdiscovery\tcheck_validity\tlos_check\tdistance_center\tvolumes\tshapes')
FREEZE = {'ERootMotionFreezeBehavior::StopOnContact': 'stop', 'ERootMotionFreezeBehavior::PauseOnContact': 'pause'}
DISCOVERY = {
    'ETargetDiscoveryBehavior::None': 'none',
    'ETargetDiscoveryBehavior::PrefersActiveTargets': 'prefers_active',
    'ETargetDiscoveryBehavior::OnlyLockedTargets': 'only_locked',
    'ETargetDiscoveryBehavior::OnlyActiveTargets': 'only_active',
    'ETargetDiscoveryBehavior::PrefersLockedTargets': 'prefers_locked',
}


def number(v):
    f = float(v or 0)
    return repr(int(f)) if f == int(f) else repr(f)


def shape(v):
    """One volume of a record as the text the table holds."""
    kind = str(v.get('id') or '').split('::')[-1]
    origin = str(v.get('Origin') or '').split('::')[-1]
    bone = str(v.get('BoneName') or 'None')
    rot = v.get('Rotation') or {}
    loc = v.get('Location') or {}
    why = None
    if kind not in ('SHAPE_Sphere', 'SHAPE_Box'):
        why = kind.replace('SHAPE_', '').lower() or 'shape'
    elif v.get('bShapeIsHollow'):
        why = 'hollow'
    elif v.get('AnimationKeys'):
        why = 'moving'
    elif not (origin == 'OFFSET_Mesh' or (origin == 'OFFSET_Bone' and bone in ('Root', 'None'))) or not str(v.get('OriginMesh') or '').endswith('ORIGIN_Character'):
        why = 'origin'
    elif kind == 'SHAPE_Box' and (rot.get('Pitch') or rot.get('Roll')):
        why = 'tilted'
    if why:
        return 'other:' + why
    los = '1' if v.get('bEnableLOSCheck') else '0'
    at = [number(loc.get('X')), number(loc.get('Y')), number(loc.get('Z'))]
    if kind == 'SHAPE_Sphere':
        return ':'.join(['sphere'] + at + [number(v.get('SphereRadius')), los])
    box = v.get('BoxExtent') or {}
    return ':'.join(['box'] + at + [number(box.get('X')), number(box.get('Y')), number(box.get('Z')), number(rot.get('Yaw')), los])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--content-db', required=True)
    ap.add_argument('--abilities', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    with open(a.abilities, encoding='utf-8') as f:
        lines = f.read().splitlines()[1:]
    guids = sorted({int(l.split('\t')[1]) for l in lines if l})
    db = sqlite3.connect('file:' + a.content_db.replace('\\', '/') + '?mode=ro', uri=True)
    rows = []
    for guid in guids:
        own = db.execute('SELECT data FROM record WHERE guid = ?', (guid,)).fetchone()
        ability = json.loads(gzip.decompress(own[0])) if own else {}
        freeze = [FREEZE.get(str(ability.get('RootMotionFreezeBehavior') or ''), 'none'), number(ability.get('RootMotionFreezeDistance')),
                  number(ability.get('RootMotionFreezeRadiusScale') if ability.get('RootMotionFreezeRadiusScale') is not None else 1), '1' if ability.get('RootMotionIgnoreNonTarget') else '0']
        for key, targeting in db.execute('SELECT key, targeting_guid FROM ab_targeting WHERE ability_guid = ? ORDER BY key', (guid,)).fetchall():
            if not targeting:
                continue
            rec = db.execute('SELECT data FROM record WHERE guid = ?', (targeting,)).fetchone()
            if not rec:
                continue
            record = json.loads(gzip.decompress(rec[0]))
            lock = record.get('LockOnInfo') or {}
            name = str(lock.get('TargetDiscoveryBehavior') or 'ETargetDiscoveryBehavior::None')
            if name not in DISCOVERY:
                print('targeting: ability %d key %s names a target discovery this tool does not know: %s' % (guid, key, name), file=sys.stderr)
                return 1
            volumes = (record.get('HurtBoxInfo') or {}).get('TargetingVolumes') or []
            rows.append('\t'.join([str(guid), str(key), number(lock.get('ValidDistance')), number(lock.get('ValidMinDistance')), number(lock.get('ValidAngle')),
                                   number(lock.get('AngleCenter')), '1' if lock.get('bPerformConeCheck') else '0'] + freeze +
                                  [DISCOVERY[name], '1' if lock.get('bCheckTargetValidity') else '0', '1' if lock.get('bPerformLoSCheck') else '0', number(lock.get('DistanceCenter')),
                                   str(len(volumes)), ';'.join(shape(v) for v in volumes)]))
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER + '\n')
        for r in rows:
            f.write(r + '\n')
    print('targeting: %d abilities, %d records' % (len(guids), len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
