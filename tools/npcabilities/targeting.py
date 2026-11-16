# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Writes npc_targeting.tsv: the targeting record of every ability that npc_abilities.tsv lists, then completes npc_abilities.tsv with the montages an ability enters
when its root motion is obstructed and lifts the reader's hold on the moves a hit window or such a montage can now land.

A targeting record says where an ability's hit lands.  It has a target part (how the target is found, and whether anything is measured at all) and volumes (shapes swept at the hit):
the target part measures its distance, angle and line of sight only when the record's validity check is on; a record with the check off hits the target wherever it stands, whatever distance
and angle it states.  The design database (content.db) keeps the records; this reads them for the abilities the creature tables name.

    python targeting.py --content-db dist/content/content.db --abilities dist/content/npc_abilities.tsv --out dist/content/npc_targeting.tsv [--montages dist/content/npc_montages.tsv]

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

Then npc_abilities.tsv gets a last column, `branches`: ';'-joined "obstructed:montage" entries for the montages an ability enters when the montage it plays is held by a body in its freeze
box (the movement event RootMotionObstructed, the one a conditional branch of the Mycellions' Launch is bound to; a branch bound to another movement event, such as walking, or one that
starts in a named section of a montage is not written).  The client plays the branch's montage by itself; the world times the hits it carries from the moment the creature is held.  A row the
reader holds for a missing hit notify ("waits for a hit notify in its montage") is made playable (unknown 0, why empty) when the ability's montage has a hit window of a key the ability has a
hit for (the `hit_windows` column of npc_montages.tsv), or one of its branch montages has a hit notify of such a key.  A hold that names another reason as well stays.

    python targeting.py --selftest [DIR]       checks that pass on a throw-away database and tables
"""
import argparse
import gzip
import json
import os
import shutil
import sqlite3
import sys
import tempfile

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


# ---- the montages an ability enters when it is obstructed, and the reader's hold ----------------------------------------------------------------------------------------

OBSTRUCTED = 16   # EAbilitySystemMovementEvent::RootMotionObstructed (2 is Walk, and so on: only this one is a body in the way of the root motion)


def ref(v):
    """The guid a design record reference holds ({'Guid': n}, {'Value': {'Guid': n}} or a '@... guid' text); 0 for none."""
    if isinstance(v, dict):
        g = v.get('Guid')
        if isinstance(g, int):
            return g
        g = ref(v.get('Value'))
        return g if isinstance(g, int) else 0
    if isinstance(v, str) and v.startswith('@'):
        x = v[1:].split()[0]
        if x.isdigit():
            return int(x)
    return 0


def branch_records(db):
    """The ability, conditional branch and animation branch records, decoded once: guid -> record.  (The record table has no index on its guid: a lookup for each is a scan of the table.)"""
    out = {}
    for guid, data in db.execute("SELECT guid, data FROM record WHERE schema IN ('AoCAbilityRecordBase', 'ConditionalBranchRecordBase', 'AnimBranchRecordBase')"):
        try:
            out[guid] = json.loads(gzip.decompress(data))
        except Exception:
            continue
    return out


def obstructed_branches(records, ability_guid):
    """The montage paths an ability enters when its root motion is obstructed: a conditional branch bound to that movement event, through its animation branch's montages that begin at their start."""
    out = []
    for entry in (records.get(ability_guid) or {}).get('ConditionalBranchesIds') or []:
        conditional = records.get(ref(entry)) or {}
        events = conditional.get('EventBindings') or {}
        if not int(events.get('MovementEvents') or 0) & OBSTRUCTED:
            continue
        for montage in (records.get(ref(conditional.get('AnimBranchId'))) or {}).get('AnimMontages') or []:
            if str(montage.get('StartSectionName') or 'None') != 'None':
                continue   # (a branch into a section of a montage: where in that montage its notifies fall is not said here)
            for path in montage.get('AbilityAnimationSoftPaths') or []:
                if path and path not in out:
                    out.append(path)
    return out


def hit_keys_of_abilities(db):
    """ability guid -> the keys of the hits its record lands itself (the hit notifies and windows of a montage name one of them)."""
    out = {}
    for guid, key in db.execute("SELECT ability_guid, key FROM ab_ability_hit WHERE source = 'hit'"):
        out.setdefault(guid, set()).add(key)
    return out


def montage_paths_of_abilities(db):
    """ability guid -> the montage paths the ability record names, in their order."""
    out = {}
    for guid, path in db.execute('SELECT ability_guid, soft_path FROM ab_montage ORDER BY ability_guid, anim_index'):
        if path:
            out.setdefault(guid, []).append(path)
    return out


def keys_of(cell, field):
    """The keys of a montage table cell of ';'-joined entries ("time:key" for the hits, "start:end:key[:interval]" for the windows); `field` is the position of the key in an entry."""
    out = set()
    for entry in cell.split(';'):
        parts = entry.split(':')
        if len(parts) > field and parts[field].lstrip('-').isdigit():
            out.add(int(parts[field]))
    return out


def read_montages(path):
    """The montage table: path -> {'hits': the keys of its hit notifies, 'windows': the keys of its hit windows}; {} when the file is not there."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    if not lines:
        return out
    head = lines[0].split('\t')
    if 'hits' not in head:
        return out
    hits_at = head.index('hits')
    windows_at = head.index('hit_windows') if 'hit_windows' in head else -1
    for line in lines[1:]:
        if not line:
            continue
        cells = line.split('\t')
        if len(cells) != len(head):
            continue
        out[cells[0]] = {'hits': keys_of(cells[hits_at], 1), 'windows': keys_of(cells[windows_at], 2) if windows_at >= 0 else set()}
    return out


def read_abilities(path):
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    if not lines:
        return [], []
    return lines[0].split('\t'), [line.split('\t') for line in lines[1:] if line]


def ability_rows_text(head, rows, db, montages):
    """The creature table with its `branches` column and the hit-notify holds lifted: the text the world reads."""
    columns = {name: i for i, name in enumerate(head)}
    with_branches = 'branches' in columns
    branches_at = columns.get('branches', len(head))
    ability_at, unknown_at, why_at = columns['ability'], columns['unknown'], columns['why']
    records, hit_keys, own_montages = branch_records(db), hit_keys_of_abilities(db), montage_paths_of_abilities(db)
    branch_cache = {}
    lines = [head if with_branches else head + ['branches']]
    for cells in rows:
        if len(cells) < len(head):
            lines.append(cells + [''] * (len(head) + (0 if with_branches else 1) - len(cells)))
            continue
        try:
            ability = int(cells[ability_at])
        except ValueError:
            ability = 0
        if ability not in branch_cache:
            branch_cache[ability] = obstructed_branches(records, ability) if ability else []
        branches = branch_cache[ability]
        if cells[unknown_at] == '1' and 'hit notify' in cells[why_at]:
            reasons = [r for r in cells[why_at].split(',') if r.strip()]
            if all('hit notify' in r for r in reasons):
                keys = hit_keys.get(ability, set())
                has_window = any(montages.get(p, {}).get('windows', set()) & keys for p in own_montages.get(ability, []))
                has_branch = any(montages.get(p, {}).get('hits', set()) & keys for p in branches)
                if has_window or has_branch:
                    cells[unknown_at] = '0'
                    cells[why_at] = ''
        cell = ';'.join('obstructed:' + p for p in branches)
        if with_branches:
            cells[branches_at] = cell
        else:
            cells.append(cell)
        lines.append(cells)
    return '\n'.join('\t'.join(cells) for cells in lines) + '\n'


def complete_abilities(db, abilities_path, montages_path):
    """Reads the creature table, completes it and writes it back in place; returns how many holds were lifted and how many rows were written."""
    head, rows = read_abilities(abilities_path)
    if not head:
        return 0, 0

    def holds(lines):
        n = 0
        for cells in lines:
            n += 1 if len(cells) > 13 and cells[10] == '1' and 'hit notify' in cells[13] else 0
        return n

    was = holds(rows)   # (counted before the rows are completed: they are changed in place)
    text = ability_rows_text(head, rows, db, read_montages(montages_path))
    out = abilities_path + '.%d.tmp' % os.getpid()   # (written beside the table: the replace stays on its own disk)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(out, abilities_path)
    now = holds([line.split('\t') for line in text.splitlines()[1:] if line])
    return was - now, len(text.splitlines()) - 1


# ---- the checks of the branch and hold pass ------------------------------------------------------------------------------------------------------------------------

def selftest(root=None):
    """A throw-away content.db and two tables: a Launch with an obstruction branch and a window move are made playable; a move whose window has no key of its hit, a branch of another
    movement event, a branch into a section and a move with another reason to wait stay as they were."""
    d = os.path.join(root or tempfile.gettempdir(), 'aoc_targeting_selftest_%d' % os.getpid())
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    db = sqlite3.connect(os.path.join(d, 'content.db'))
    db.execute('CREATE TABLE record (guid INTEGER PRIMARY KEY, name TEXT, schema TEXT, data BLOB)')
    db.execute('CREATE TABLE ab_montage (ability_guid INTEGER, anim_index INTEGER, soft_path TEXT)')
    db.execute('CREATE TABLE ab_ability_hit (ability_guid INTEGER, source TEXT, key INTEGER)')

    def add(guid, name, schema, data):
        db.execute('INSERT INTO record VALUES (?, ?, ?, ?)', (guid, name, schema, gzip.compress(json.dumps(data).encode())))

    def branch_to(ability, number, events, path, section='None'):
        add(ability + 1, 'Cond%d' % number, 'ConditionalBranchRecordBase', {'AnimBranchId': {'Guid': ability + 2}, 'EventBindings': events, 'BranchTrigger': ''})
        add(ability + 2, 'Branch%d' % number, 'AnimBranchRecordBase', {'AnimMontages': [{'AbilityAnimationSoftPaths': [path], 'StartSectionName': section, 'TargetingKey': 1}]})
        add(ability, 'Ability%d' % number, 'AoCAbilityRecordBase', {'ConditionalBranchesIds': [{'Key': 1, 'Value': {'Guid': ability + 1}}]})

    impact = '/Game/Test/Launch_Impact.Launch_Impact'
    launch, walks, section, windowed, wrong_key, held, two_reasons = 100, 110, 120, 130, 140, 150, 160
    branch_to(launch, 1, {'MovementEvents': 16, 'HitEvents': 4}, impact)
    branch_to(walks, 2, {'MovementEvents': 2}, impact)
    branch_to(section, 3, {'MovementEvents': 16}, impact, 'HIT')
    for ability, keys in ((launch, (1, 2)), (walks, (1,)), (section, (1,)), (windowed, (1,)), (wrong_key, (0,)), (held, (0,)), (two_reasons, (0,))):
        for key in keys:
            db.execute("INSERT INTO ab_ability_hit VALUES (?, 'hit', ?)", (ability, key))
    for ability, path in ((launch, '/Game/Test/Launch.Launch'), (windowed, '/Game/Test/Window.Window'), (wrong_key, '/Game/Test/WrongKey.WrongKey'), (two_reasons, '/Game/Test/Window.Window')):
        db.execute('INSERT INTO ab_montage VALUES (?, 0, ?)', (ability, path))
    db.commit()
    montages = os.path.join(d, 'npc_montages.tsv')
    with open(montages, 'w', encoding='utf-8', newline='\n') as f:
        f.write('montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable\tanim_end\tfires\thit_windows\n')
        f.write('/Game/Test/Launch.Launch\t1\tp\t1\t2.7\t1\t\t\t0:0:0:0;1.2:100:0:0\t0\t\t\t\n')
        f.write(impact + '\t2\tp\t2\t1.4\t1\t\t0.038:1\t\t0\t\t\t\n')
        f.write('/Game/Test/Window.Window\t3\tp\t3\t2.0\t1\t\t\t\t0\t\t\t0.3:0.9:1:0.25\n')   # (a window of key 1 that lands again every 0.25 s)
        f.write('/Game/Test/WrongKey.WrongKey\t4\tp\t4\t2.0\t1\t\t\t\t0\t\t\t0.3:0.9:5\n')   # (a window of a key the ability has no hit for)
    abilities = os.path.join(d, 'npc_abilities.tsv')
    head = ('archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate\tarrival\tnext')
    hold = 'waits for a hit notify in its montage (%s)'
    with open(abilities, 'w', encoding='utf-8', newline='\n') as f:
        f.write(head + '\n')
        for ability, name, why in ((launch, 'Launch', hold % 'Launch'), (walks, 'Walks', hold % 'Walks'), (section, 'Section', hold % 'Section'), (windowed, 'Windowed', hold % 'Windowed'),
                                   (wrong_key, 'WrongKey', hold % 'WrongKey'), (held, 'Held', hold % 'Held'), (two_reasons, 'TwoReasons', 'AoCSTC_ExpressionEval,' + hold % 'TwoReasons')):
            f.write('Test/%s.Default__%s_C\t%d\t%s\t12\t1\t16\t-1\t600\t90\t\t1\t1\tc0:tt(gt,12)\t%s\t\t\t\t\n' % (name, name, ability, name, why))
    complete_abilities(db, abilities, montages)
    text = open(abilities, encoding='utf-8').read()
    lines = text.splitlines()
    problems = []
    if lines[0].split('\t')[-1] != 'branches':
        problems.append('the branches column is not last')
    wants = {  # ability -> (played now, its branches)
        launch: (True, 'obstructed:' + impact), walks: (False, ''), section: (False, ''), windowed: (True, ''), wrong_key: (False, ''), held: (False, ''), two_reasons: (False, ''),
    }
    seen = set()
    for line in lines[1:]:
        c = line.split('\t')
        ability = int(c[1])
        played, branches = wants[ability]
        seen.add(ability)
        if (c[10] == '0') != played or (played and c[13] != '') or (not played and 'hit notify' not in c[13]):
            problems.append('ability %d: unknown %s, why "%s" (played expected %s)' % (ability, c[10], c[13], played))
        if c[-1] != branches:
            problems.append('ability %d: branches "%s", expected "%s"' % (ability, c[-1], branches))
    if seen != set(wants):
        problems.append('rows missing: %s' % sorted(set(wants) - seen))
    montage_keys = read_montages(montages)
    if montage_keys.get('/Game/Test/Window.Window', {}).get('windows') != {1}:
        problems.append('a window with an interval is not read as key 1: %s' % montage_keys.get('/Game/Test/Window.Window'))
    db.close()
    shutil.rmtree(d, ignore_errors=True)
    for p in problems:
        print('targeting selftest: FAIL:', p)
    if not problems:
        print('targeting selftest: ok: the Launch (obstruction branch) and the window move are played now, the wrong window key, the walking branch, the branch into a section and the second reason stay held, the branches column is last')
    return 0 if not problems else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--content-db')
    ap.add_argument('--abilities')
    ap.add_argument('--out')
    ap.add_argument('--montages', default='')
    ap.add_argument('--selftest', nargs='?', const='', default=None)
    a = ap.parse_args()
    if a.selftest is not None:
        return selftest(a.selftest or None)
    if not (a.content_db and a.abilities and a.out):
        ap.error('--content-db, --abilities and --out are required')
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
    lifted, written = complete_abilities(db, a.abilities, a.montages or os.path.join(os.path.dirname(a.abilities), 'npc_montages.tsv'))
    print('targeting: %d hit-notify hold(s) lifted, %d rows written with their branches' % (lifted, written))
    db.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
