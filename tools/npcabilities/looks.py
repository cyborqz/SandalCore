# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Writes npc_appearances.tsv and npc_names.tsv: what dresses and names the people (humanoid NPCs) of the world.

A humanoid's body is not its Blueprint's mesh: the server merges it from an NPCAppearance record (a shared appearance info, the outfits it may wear and how
likely each is) when the NPC is made, and the NPC's name is the first non-empty list of display names of its instance, its asset set's entry for it and its asset.
The design database (content.db) keeps the records; this flattens the ones the people use.

    python looks.py --content-db dist/content/content.db --out-dir dist/content

npc_appearances.tsv, one record per line, the first field says which (sorted within each kind):
  A  guid  name  shared_info  randomize_custom  randomize_race_gender  hair_natural  seed  curve  outfits  preset  body_shared
       an NPCAppearance: `outfits` are the guids of its outfit options in order, `curve` the weight curve that picks one (empty: equal weights), `preset` the guid of the
       customization preset the record pins (0 for none; the game uses it only where the record does not randomize its customization), `body_shared` the shared appearance
       info that holds the preset's race and gender, for a record whose NPCs wear the preset: the record's own when it holds them, else the commonest one that equals it in
       every member but its race and gender map and does (0: not a preset record, or no such info, so the NPC keeps its own race and gender)
  O  guid  name  single_curve  curve
       a HumanoidOutfit: `single_curve` 1 = `curve` picks the item of every slot (the data's rule for every outfit)
  S  outfit  slot  curve
       a slot's own curve, only where an outfit has one (used when `single_curve` is 0)
  I  outfit  slot  items
       the item appearance records of one slot of an outfit, in list order (slot numbers are EItemEquipSlot: 0 head, 2 chest, 3 wrists, 4 hands, 5 legs, 6 feet,
       7 shoulders, 8 back, 9 waist, 15 primary held item, 16 secondary held item)
  R  shared_info  name  pairs
       the races and genders a shared appearance info may make: `race:gender,...` (ECharacterRace number, ECharacterGender number)
  P  preset  name  customization  lowercase_morphs  race  gender
       a customization preset an NPCAppearance that does not randomize pins (its saved face, hair, eyes, colours, decals and body sections), as the client's customization JSON
       (camelCase members; sectionsValues and faceMorphWeightMaps as objects), without race and gender, which are the last two fields (ECharacterRace and ECharacterGender
       numbers): an NPC that wears the preset takes them, so that its face and its body match.  A preset with no seed of its own
       is the whole difference from the client's all-zero default, so a member that is zero is left out; one with a seed is written in full.  `lowercase_morphs` are the
       face morph names (joined by ',') that start with a small letter, which that JSON cannot tell from a capital one.  A preset whose ids are not records is left out.
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


# The 30 scalar members of a customization in wire order (the client's names), the decal and blend group members, as the client's customization JSON spells them.
PRESET_SCALARS = ['SkinColorHue', 'SkinColorPigmentation', 'NormalDetailStrength01', 'NormalDetailStrength02', 'SkinSet', 'EyeColor', 'EyeShape', 'ScleraShape', 'Eyebrows',
                  'HeadHair', 'HeadHairRootColor', 'HeadHairTipColor', 'HeadHairLength', 'HeadHairContrast', 'HeadHairGradient', 'FacialHairLip', 'FacialHairChin',
                  'FacialHairCheek', 'FacialHairLipLength', 'FacialHairChinLength', 'FacialHairCheekLength', 'EyelashLength', 'FacialHairRootColor', 'FacialHairTipColor',
                  'FacialHairContrast', 'FacialHairGradient', 'RacialHorns', 'RacialHornsLength', 'NailColor', 'NailOpacity']
DECAL_MEMBERS = [('GUID', 'guid'), ('Strength', 'strength'), ('ScaleU', 'scaleU'), ('ScaleV', 'scaleV'), ('SlotIndex', 'slotIndex'),
                 ('MergeSectionMaterialBase', 'mergeSectionMaterialBase'), ('OffsetU', 'offsetU'), ('OffsetV', 'offsetV'), ('Angle', 'angle'), ('Color', 'color'),
                 ('Discoloration', 'discoloration')]
BLEND_MEMBERS = [('DecalAppearanceGuid', 'decalAppearanceGuid'), ('ColorGuid', 'colorGuid'), ('Intensity', 'intensity')]


def lower_first(s):
    return s[:1].lower() + s[1:]


PRESET_IDS = {'EyeColor', 'EyeShape', 'ScleraShape', 'Eyebrows', 'HeadHair', 'HeadHairRootColor', 'HeadHairTipColor', 'FacialHairLip', 'FacialHairChin', 'FacialHairCheek',
              'FacialHairRootColor', 'FacialHairTipColor', 'RacialHorns', 'NailColor'}   # the members that name a record (an id)


def preset_json(save):
    """The customization a preset record saves, as the client's customization JSON text (no race, gender or class: the NPC's own race and gender are used), and the morph
    names that start with a small letter (the client's JSON cannot tell them from the capital ones, so they are listed beside it)."""
    seed = int(save.get('RandomSeed') or 0)
    out = {}
    if seed:
        out['randomSeed'] = seed
    for key in PRESET_SCALARS:
        v = save.get(key) or 0
        if key in PRESET_IDS and v and int(v) < 10 ** 12:
            raise ValueError('%s %s is not a record id' % (key, v))   # (a placeholder the data never finished: the NPC keeps the face the game's own pool gives it)
        if v or seed:
            out[lower_first(key)] = v
    decals = [{j: (d.get(k) or 0) for k, j in DECAL_MEMBERS} for d in save.get('DecalData') or []]
    blends = [{j: (g.get(k) or 0) for k, j in BLEND_MEMBERS} for g in save.get('DecalBlendGroups') or []]
    sections = {}
    for e in save.get('SectionsValues') or []:
        v = e.get('Value') or {}
        sections[lower_first(str(e['Key']).split('::')[-1])] = {'x': v.get('X') or 0, 'y': v.get('Y') or 0, 'z': v.get('Z') or 0}
    morphs = {}
    lowers = []
    for e in save.get('FaceMorphWeightMaps') or []:
        name = str(e['Key'])
        if not name or lower_first(name) in morphs or '\t' in name or ',' in name:
            raise ValueError('a morph name the client JSON cannot carry: ' + name)
        if name[0].islower():
            lowers.append(name)
        morphs[lower_first(name)] = e.get('Value') or 0
    if len(sections) != len(save.get('SectionsValues') or []):
        raise ValueError('a body section named twice')
    for member, value in (('decalData', decals), ('decalBlendGroups', blends), ('sectionsValues', sections), ('faceMorphWeightMaps', morphs)):
        if value or seed:
            out[member] = value
    return json.dumps(out, separators=(',', ':'), ensure_ascii=True), ','.join(lowers)


def preset_body(save):
    """The (race, gender) numbers a preset record saves (ECharacterRace and ECharacterGender), or a ValueError."""
    race = RACES.get(str(save.get('Race')).split('::')[-1])
    gender = {'Male': 0, 'Female': 1}.get(str(save.get('Gender')).split('::')[-1])
    if race is None or gender is None:
        raise ValueError('no race or gender: %s %s' % (save.get('Race'), save.get('Gender')))
    return race, gender


def map_pairs(m):
    """The (race, gender) pairs a RaceGenderAppearanceMap holds a record for (a member such as MaleVaeluneId that names one)."""
    pairs = set()
    if m:
        for key, v in m[1].items():
            if not (isinstance(v, dict) and v.get('Guid')):
                continue
            gender = 0 if key.startswith('Male') else 1 if key.startswith('Female') else -1
            race = RACES.get(key[len('Male'):-2] if gender == 0 else key[len('Female'):-2] if gender == 1 else '', -1)
            if gender >= 0 and race >= 0:
                pairs.add((race, gender))
    return sorted(pairs)


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
    app_lines, outfit_lines, slot_lines, item_lines, race_lines, preset_lines = [], [], [], [], [], []
    shared_used = set()
    preset_used = set()
    npc_rows = list(rows_of(con, 'NPCAppearance'))
    for guid, name, d in npc_rows:
        pin = int((d.get('CustomizationPresetId') or {}).get('Guid') or 0)
        if pin and d['SharedAppearanceInfoId']['Guid'] and not d['bRandomizeCustomization']:
            preset_used.add(pin)   # (a record that randomizes its customization never uses the preset it pins)
    # the presets those records pin: their customization and body (a preset whose ids are not records is left out)
    skipped_presets = 0
    preset_bodies = {}
    for guid, name, d in rows_of(con, 'CustomizationPreset'):
        if guid not in preset_used:
            continue
        try:
            save = d.get('Save') or {}
            text, lowers = preset_json(save)
            race, gender = preset_body(save)
            preset_lines.append('\t'.join(['P', str(guid), clean(name), text, lowers, str(race), str(gender)]))
            preset_bodies[guid] = (race, gender)
        except ValueError as e:
            skipped_presets += 1
            print('looks: preset %s left out: %s' % (name, e))
    # the shared appearance infos and the bodies (race, gender) their maps can draw: the client draws a body only for a pair its record's map names
    maps = {g: (n, d) for g, n, d in rows_of(con, 'RaceGenderAppearanceMap')}
    infos = {g: (n, d) for g, n, d in rows_of(con, 'AppearanceInfo')}
    info_pairs = {g: map_pairs(maps.get(d['RaceGenderAppearanceMapId']['Guid'])) for g, (n, d) in infos.items()}
    info_uses = {}
    for guid, name, d in npc_rows:
        sg = d['SharedAppearanceInfoId']['Guid']
        info_uses[sg] = info_uses.get(sg, 0) + 1

    def info_rest(g):
        return {k: v for k, v in infos[g][1].items() if k != 'RaceGenderAppearanceMapId'}

    def body_shared_of(sg, body):
        """The shared info to send for an NPC that wears a preset with this body: the record's own when its map holds the body, else the commonest info that equals it in
        every member but the map and holds it; 0 when there is none."""
        if body in info_pairs.get(sg, ()):
            return sg
        if sg not in infos:
            return 0
        rest = info_rest(sg)
        found = [g for g in infos if g != sg and body in info_pairs[g] and info_rest(g) == rest]
        found.sort(key=lambda g: (-info_uses.get(g, 0), g))
        return found[0] if found else 0

    body_moves = 0
    for guid, name, d in npc_rows:
        sg = d['SharedAppearanceInfoId']['Guid']
        outs = ','.join(str(o['Guid']) for o in d['HumanoidOutfitOptionsIds'])
        pin = int((d.get('CustomizationPresetId') or {}).get('Guid') or 0)
        body_shared = 0
        if pin in preset_bodies and sg and not d['bRandomizeCustomization']:
            body_shared = body_shared_of(sg, preset_bodies[pin])
            if body_shared == 0:
                print('looks: record %s: no shared appearance info holds its preset\'s race and gender, its NPCs keep their own' % name)
            elif body_shared != sg:
                body_moves += 1
                shared_used.add(body_shared)
        shared_used.add(sg)
        app_lines.append('\t'.join(['A', str(guid), clean(name), str(sg), str(int(bool(d['bRandomizeCustomization']))), str(int(bool(d['bRandomizeRaceAndGender']))),
                                    str(int(bool(d['bIsHairNatural']))), str(int(d['CustomizationSeed'])), curve_text(d['OutfitWeightCurve']), outs, str(pin), str(body_shared)]))
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
    for guid, (name, d) in sorted(infos.items()):
        if guid not in shared_used:
            continue
        race_lines.append('\t'.join(['R', str(guid), clean(name), ','.join('%d:%d' % p for p in info_pairs[guid])]))
    os.makedirs(a.out_dir, exist_ok=True)
    body = ['# npc_appearances.tsv: see tools/npcabilities/looks.py']
    for part in (app_lines, outfit_lines, slot_lines, item_lines, race_lines, preset_lines):
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
    print('looks: %d appearances, %d outfits, %d item lists, %d slot curves, %d race maps, %d presets (%d left out; %d records need another shared info for the body), %d names' % (
        len(app_lines), len(outfit_lines), len(item_lines), len(slot_lines), len(race_lines), len(preset_lines), skipped_presets, body_moves, len(names) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
