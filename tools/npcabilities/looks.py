# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Writes npc_appearances.tsv and npc_names.tsv: what dresses and names the people (humanoid NPCs) of the world.

A humanoid's body is not its Blueprint's mesh: the server merges it from an NPCAppearance record (a shared appearance info, the outfits it may wear and how
likely each is) when the NPC is made, and the NPC's name is the first non-empty list of display names of its instance, its asset set's entry for it and its asset.
The design database (content.db) keeps the records; this flattens the ones the people use.

    python looks.py --content-db dist/content/content.db --out-dir dist/content

npc_appearances.tsv, one record per line, the first field says which (sorted within each kind):
  A  guid  name  shared_info  randomize_custom  randomize_race_gender  hair_natural  seed  curve  outfits
       an NPCAppearance: `outfits` are the guids of its outfit options in order, `curve` the weight curve that picks one (empty: equal weights)
  O  guid  name  single_curve  curve
       a HumanoidOutfit: `single_curve` 1 = `curve` picks the item of every slot (the data's rule for every outfit)
  S  outfit  slot  curve
       a slot's own curve, only where an outfit has one (used when `single_curve` is 0)
  I  outfit  slot  items
       the item appearance records of one slot of an outfit, in list order (slot numbers are EItemEquipSlot: 0 head, 2 chest, 3 wrists, 4 hands, 5 legs, 6 feet,
       7 shoulders, 8 back, 9 waist, 15 primary held item, 16 secondary held item)
  R  shared_info  name  pairs
       the races and genders a shared appearance info may make: `race:gender,...` (ECharacterRace number, ECharacterGender number)
A curve is `time:value:mode` entries joined by ',' (mode L linear, C cubic, K constant); the engine's tangents are all zero in the data, so a cubic key pair is
a smooth step.  The data's curves are constant outside their keys.

npc_names.tsv, one display name per line: kind (instance, set or asset), the record's guid, the member (the asset's guid for a set's entry, else 0) and the text.
"""
import argparse
import gzip
import json
import os
import sqlite3
import sys

RACES = {'Kaelar': 0, 'Vaelune': 1, 'Vek': 2, 'Renkai': 3, 'Empyrean': 4, 'Pyrai': 5, 'Dunir': 6, 'Nikua': 7, 'Tulnar': 8}
SLOTS = [('HeadIds', 0), ('ChestIds', 2), ('WristsIds', 3), ('HandsIds', 4), ('LegsIds', 5), ('FeetIds', 6), ('ShouldersIds', 7), ('BackIds', 8), ('WaistIds', 9),
         ('PrimaryHeldItemIds', 15), ('SecondaryHeldItemIds', 16)]
MODES = {'ERichCurveInterpMode::RCIM_Linear': 'L', 'ERichCurveInterpMode::RCIM_Cubic': 'C', 'ERichCurveInterpMode::RCIM_Constant': 'K'}


def num(v):
    f = float(v)
    return repr(int(f)) if f == int(f) else repr(f)


def curve_text(cv):
    """A FRuntimeFloatCurve as the text the tables hold ('' for no keys)."""
    if not isinstance(cv, dict):
        return ''
    keys = (cv.get('EditorCurveData') or {}).get('Keys') or []
    return ','.join('%s:%s:%s' % (num(k['Time']), num(k['Value']), MODES.get(k.get('InterpMode'), 'K')) for k in keys)


def clean(s):
    return ' '.join(str(s).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ').split())


def texts(lst):
    """The display strings of an FText list, as the server reads them (the base history's source, else the key)."""
    out = []
    for t in lst or []:
        s = t.get('Source') or t.get('Key') or ''
        out.append(clean(s))
    return out


def rows_of(con, schema):
    ti = con.execute('SELECT table_index FROM record_table WHERE schema = ?', (schema + 'RecordBase',)).fetchone()
    if ti is None:
        raise SystemExit('content.db has no %s table' % schema)
    for guid, name, data in con.execute('SELECT guid, name, data FROM record WHERE table_index = ? ORDER BY guid', (ti[0],)):
        yield guid, name, json.loads(gzip.decompress(data).decode('utf-8'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--content-db', required=True)
    ap.add_argument('--out-dir', required=True)
    a = ap.parse_args()
    con = sqlite3.connect('file:' + a.content_db.replace('\\', '/') + '?mode=ro', uri=True)
    app_lines, outfit_lines, slot_lines, item_lines, race_lines = [], [], [], [], []
    shared_used = set()
    for guid, name, d in rows_of(con, 'NPCAppearance'):
        sg = d['SharedAppearanceInfoId']['Guid']
        shared_used.add(sg)
        outs = ','.join(str(o['Guid']) for o in d['HumanoidOutfitOptionsIds'])
        app_lines.append('\t'.join(['A', str(guid), clean(name), str(sg), str(int(bool(d['bRandomizeCustomization']))), str(int(bool(d['bRandomizeRaceAndGender']))),
                                    str(int(bool(d['bIsHairNatural']))), str(int(d['CustomizationSeed'])), curve_text(d['OutfitWeightCurve']), outs]))
    for guid, name, d in rows_of(con, 'HumanoidOutfit'):
        single = bool(d['bUseSingleWeightCurve'])
        outfit_lines.append('\t'.join(['O', str(guid), clean(name), str(int(single)), curve_text(d['WeightCurve'])]))
        for field, slot in SLOTS:
            ids = [str(x['Guid']) for x in d.get(field) or []]
            if ids:
                item_lines.append('\t'.join(['I', str(guid), str(slot), ','.join(ids)]))
            c = curve_text(d.get(field.replace('Ids', 'WeightCurve')))
            if c:
                slot_lines.append('\t'.join(['S', str(guid), str(slot), c]))
    maps = {g: (n, d) for g, n, d in rows_of(con, 'RaceGenderAppearanceMap')}
    for guid, name, d in rows_of(con, 'AppearanceInfo'):
        if guid not in shared_used:
            continue
        m = maps.get(d['RaceGenderAppearanceMapId']['Guid'])
        pairs = []
        if m:
            for key, v in m[1].items():
                if not (isinstance(v, dict) and v.get('Guid')):
                    continue
                gender = 0 if key.startswith('Male') else 1 if key.startswith('Female') else -1
                race = RACES.get(key[len('Male'):-2] if gender == 0 else key[len('Female'):-2] if gender == 1 else '', -1)
                if gender >= 0 and race >= 0:
                    pairs.append((race, gender))
        race_lines.append('\t'.join(['R', str(guid), clean(name), ','.join('%d:%d' % p for p in sorted(pairs))]))
    os.makedirs(a.out_dir, exist_ok=True)
    body = ['# npc_appearances.tsv: see tools/npcabilities/looks.py']
    for part in (app_lines, outfit_lines, slot_lines, item_lines, race_lines):
        body.extend(part)
    names = ['kind\tsource\tmember\ttext']
    for guid, nm, d in rows_of(con, 'PopulationInstance'):
        for t in texts(d['DisplayNameOverrides']):
            names.append('\t'.join(['instance', str(guid), '0', t]))
    for guid, nm, d in rows_of(con, 'PopulationAsset'):
        for t in texts(d['DisplayNameOverrides']):
            names.append('\t'.join(['asset', str(guid), '0', t]))
    for guid, nm, d in rows_of(con, 'AssetSet'):
        for e in d['AssetDefMap']:
            for t in texts((e.get('Value') or {}).get('DisplayNameOverrides')):
                names.append('\t'.join(['set', str(guid), str(e['Key']['Guid']), t]))
    for fname, lines in (('npc_appearances.tsv', body), ('npc_names.tsv', names)):
        path = os.path.join(a.out_dir, fname)
        tmp = path + '.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        os.replace(tmp, path)
    print('looks: %d appearances, %d outfits, %d item lists, %d slot curves, %d race maps, %d names' % (
        len(app_lines), len(outfit_lines), len(item_lines), len(slot_lines), len(race_lines), len(names) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
