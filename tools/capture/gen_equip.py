# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""Writes server/worldd/tests/capture_equip.h: the retail equip / unequip exchanges the research decoded.

Needs the research's decoded captures: a folder (--cc DIR or AOC_RESEARCH_CC) with flat/<id>.jsonl, one record per RPC or property of a retail capture, made by the research's decoder from the PCAPRepo files (neither is in the repository).  Usage:
    python tools/capture/gen_equip.py OUT.h [content.db] [--cc DIR] [--check]    (OUT.h is server/worldd/tests/capture_equip.h; content.db defaults to dist/content/content.db;
                                                                                   --check writes nothing and exits 1 when the header would change)

For each scenario (one retail capture of the equip_unequip folder or the equipping-all one) it writes
  * the requests the client sent (ServerItemUsed: storage, slot, the instance guid),
  * the ClientReceiveStorageEvent calls the server answered with, in order (type, which storage, slot, item record and guid, whether
    the event carries the character as instigator),
  * the state the answer needs: the items the events name, the bag around them (fillers below the first free slot) and the visible gear
    the capture's HeldItems and AppearanceIDs show, with each record's slots and two-handedness from content.db,
  * what the pawn showed after: the 23 HeldItems records and the AppearanceIDs (slot, record), and how far the HeldItems array key moved
    (the capture's key minus its base).
"""
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CC = os.environ.get('AOC_RESEARCH_CC', '')

SCENARIOS = [
    (1219, 'equip a chest piece into an empty slot'),
    (1253, 'take the chest piece off'),
    (1204, 'equip a two-hander over a main hand and an off hand'),
    (1205, 'equip a two-handed bow over a wand and a focus'),
    (1305, 'swap one artisan shirt for another'),
    (1223, 'equip an earring into the left ear'),
    (1224, 'equip an earring into the right ear'),
    (1235, 'equip a ring into the right hand'),
    (1236, 'equip a second ring into the left hand'),
    (1229, 'equip a one-handed axe'),
    (1230, 'equip a two-handed sword'),
    (1228, 'equip a shield'),
    (1233, 'equip a wand'),
    (1213, 'equip an artisan shirt'),
    (1222, 'equip a full costume'),
    (1221, 'equip a cosmetic cloak'),
    (1262, 'take a shield off'),
    (1264, 'take a two-hander off'),
    (1267, 'take a wand off'),
    (1278, 'equip all 18 pieces of a tank, one request each'),
]

HELD_KEYS = [15, 16, 17, 18, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 41, 75, 76, 77, 78]
SHOWN = set(range(1, 10)) | {71, 79}


def hexguid(g):
    return '%08x%08x%08x%08x' % tuple(g)

# -- C | Y | B . C-O-A --

def load(cid):
    reqs, events, held, appearance = [], [], None, None
    own_ch = None
    held_delta = None
    app_state = {}
    held_updates = []
    for l in open(os.path.join(CC, 'flat', '%d.jsonl' % cid)):
        r = json.loads(l)
        v = r.get('v')
        if r['name'] == 'ServerItemUsed':
            own_ch = r['ch']
            d = dict(v)
            g = d['ExpectedItem']
            reqs.append((d['Storage'], d['Slot'], hexguid([g['ExpectedItem.A'], g['ExpectedItem.B'], g['ExpectedItem.C'], g['ExpectedItem.D']])))
        elif r['name'] == 'ClientReceiveStorageEvent' and v:
            e = dict(v)['ItemEvent']
            it = e['item']
            t = e['targets'][0]
            events.append(dict(type=e['type'], obj=t['obj'], slot=t['slots'][0][0], record=it['ItemRecordId'], guid=hexguid(it['GUID']),
                               instig=any(e['instigator']), quality=it['Quality'], durability=it['Durability'], created=it['CreationTime'],
                               quantity=it['Quantity']))
        elif r['name'] == 'HeldItems' and own_ch in (None, r['ch']):
            held = {x[1]['Items.Key']: x[1]['Items.Value.ItemGuid'] for x in v}
            # a capture may hold several updates: the array key moved from the first one's base to the last one's key
            held_delta = (r['base'] if held_delta is None else held_delta[0], r['key'])
            held_updates.append((r['base'], r['key']))
        elif r['name'] == 'AppearanceIDs' and own_ch in (None, r['ch']):
            # a dynamic array's update carries the elements that changed and the new length: keep the array as the deltas build it
            state = app_state.setdefault('a', {})
            for idx, x in v['items'].items():
                state[int(idx)] = (x['AppearanceIDs'][3], x['AppearanceIDs'][0])
            appearance = [state[i] for i in sorted(state)]
    if len(held_updates) > 1 and False:
        print(held_updates)
    return reqs, events, held, held_delta, appearance


def main(out_path, db_path, check=False):
    con = sqlite3.connect('file:%s?mode=ro' % db_path, uri=True)
    infos = {}

    def info(record):
        if record not in infos:
            row = con.execute('select can_equip, occupies_all_equip_slots, name from item where guid = ?', (record,)).fetchone()
            slots = [s for (s,) in con.execute('select slot from item_equip_slot where item_guid = ? order by ord', (record,))]
            infos[record] = (slots, bool(row[1]) if row else False, row[2] if row else '?')
        return infos[record]

    lines = []
    scenarios = []
    for cid, title in SCENARIOS:
        reqs, events, held, held_delta, appearance = load(cid)
        if not events or not reqs:
            print('skipping', cid)
            continue
        # which object is the bag and which the equipment
        first = reqs[0]
        bag_obj = equip_obj = None
        for e in events:
            if first[0] == 3 and e['type'] == 1 and e['slot'] == first[1] and e['guid'] == first[2]:
                bag_obj = e['obj']
            if first[0] == 4 and e['type'] == 1 and e['slot'] == first[1] and e['guid'] == first[2]:
                equip_obj = e['obj']
        objs = sorted({e['obj'] for e in events})
        if bag_obj is None and equip_obj is not None:
            bag_obj = [o for o in objs if o != equip_obj][0]
        if equip_obj is None and bag_obj is not None:
            equip_obj = [o for o in objs if o != bag_obj][0]
        assert bag_obj and equip_obj and bag_obj != equip_obj, cid
        kind = (lambda b: (lambda o: 0 if o == b else 1))(bag_obj)
        # the state before: what the events take out of a storage was there; the bag below the first free slot is full; what the capture
        # shows worn after, and no event added, was worn before
        before = {}   # (container, slot) -> (record, guid)
        added = set()
        for e in events:
            if e['type'] == 1:
                before[(kind(e['obj']), e['slot'])] = (e['record'], e['guid'])
        for e in events:
            if e['type'] == 0:
                added.add((kind(e['obj']), e['slot']))
        # the items shown after that no event named: worn before as well
        shown_after = {}
        if held is not None:
            for k, rec in held.items():
                if rec:
                    shown_after[(1, k)] = rec
        if appearance is not None:
            for s, rec in appearance:
                shown_after[(1, s)] = rec
        else:
            # no AppearanceIDs update: the armor shown is unchanged, and it is not in the capture: none is assumed
            pass
        n = 0
        for (c, s), rec in sorted(shown_after.items()):
            if (c, s) in added:
                continue
            if (c, s) not in before:
                n += 1
                before[(c, s)] = (rec, '%032x' % (0xB000000000000000000000000000 + cid * 1000 + n))
        # an equip the server put into a slot that is not the first of the record's list found the slots before it taken
        for e in events:
            if e['type'] == 0 and kind(e['obj']) == 1:
                slots, two, name = info(e['record'])
                if two or e['slot'] not in slots:
                    continue
                for s in slots[:slots.index(e['slot'])]:
                    if (1, s) not in before and (1, s) not in added:
                        n += 1
                        before[(1, s)] = (e['record'], '%032x' % (0xC000000000000000000000000000 + cid * 1000 + n))
        add_slots = [e['slot'] for e in events if e['type'] == 0 and kind(e['obj']) == 0]
        removed_bag = {e['slot'] for e in events if e['type'] == 1 and kind(e['obj']) == 0}
        fillers = []
        if add_slots:
            for s in range(0, max(add_slots)):
                if s in add_slots or s in removed_bag or (0, s) in before:
                    continue
                fillers.append(s)
        for i, s in enumerate(fillers):
            before[(0, s)] = (0, '%032x' % (0xF000000000000000000000000000 + cid * 1000 + i))   # record 0: a filler with no record
        records = set(rec for (rec, g) in before.values() if rec) | set(e['record'] for e in events)
        scenarios.append(dict(cid=cid, title=title, reqs=reqs, events=events, kind=kind, before=before, held=held, held_delta=held_delta,
                              appearance=appearance, records=records))
        for rec in records:
            info(rec)
# CoA : C Y B

    out = []
    w = out.append
    w('// ============================================================================')
    w('//  server/worldd/tests/capture_equip.h')
    w('//')
    w('//  GENERATED by tools/capture/gen_equip.py from the research\'s decoded retail captures (PCAPRepo equip_unequip/*,')
    w('//  ids 1204-1305, and the tank\'s equipping_all_items); do not edit.')
    w('//')
    w('//  The retail server\'s answers to ServerItemUsed: the requests the client sent, the ClientReceiveStorageEvent calls that came back, in')
    w('//  order, and what the pawn showed after (HeldItems, AppearanceIDs: a dynamic array update lists the elements that changed, so `shown` is those, merged')
    w('//  over the capture; how far the HeldItems array key moved).  `before` is the state the')
    w('//  answer needs: what the events take out of a storage, the bag below the first free slot (a filler has record 0), and what the')
    w('//  capture shows worn after that no event added.  Slots and two-handedness of each record are content.db\'s (item_equip_slot).')
    w('// ============================================================================')
    w('#pragma once')
    w('')
    w('#include <cstddef>')
    w('#include <cstdint>')
    w('')
    w('namespace capture_equip {')
    w('')
    w('struct Placed { int container; int slot; int64_t record; const char* guid; };   // container 0 the Backpack, 1 the Equipment')
    w('struct Request { int storage; int slot; const char* guid; };                       // EItemLocation: Backpack 3, Equipment 4')
    w('struct Event { int type; int container; int slot; int64_t record; const char* guid; bool instigated; };   // type 0 Add, 1 Remove')
    w('struct Held { int key; int64_t record; };')
    w('struct Shown { int slot; int64_t record; };')
    w('struct Record { int64_t record; const char* name; int nslots; int slots[4]; bool two_handed; };')
    w('')
    for sc in scenarios:
        p = 'k%d' % sc['cid']
        w('// capture %d: %s' % (sc['cid'], sc['title']))
        w('inline constexpr Placed %s_before[] = {' % p)
        for (c, s), (rec, g) in sorted(sc['before'].items()):
            w('    {%d, %d, %dll, "%s"},' % (c, s, rec, g))
        w('};')
        w('inline constexpr Request %s_requests[] = {' % p)
        for st, sl, g in sc['reqs']:
            w('    {%d, %d, "%s"},' % (st, sl, g))
        w('};')
        w('inline constexpr Event %s_events[] = {' % p)
        for e in sc['events']:
            w('    {%d, %d, %d, %dll, "%s", %s},' % (e['type'], sc['kind'](e['obj']), e['slot'], e['record'], e['guid'], 'true' if e['instig'] else 'false'))
        w('};')
        held = sc['held']
        w('inline constexpr Held %s_held[] = {' % p)
        for k in HELD_KEYS:
            w('    {%d, %dll},' % (k, held.get(k, 0)))
        w('};')
        w('inline constexpr Shown %s_shown[] = {' % p)
        for s, rec in (sc['appearance'] or []):
            w('    {%d, %dll},' % (s, rec))
        w('    {-1, 0ll},   // (an array of no size is no array: this is never read)')
        w('};')
        w('inline constexpr size_t %s_nshown = %d;' % (p, len(sc['appearance'] or [])))
        w('inline constexpr bool %s_has_appearance = %s;' % (p, 'true' if sc['appearance'] is not None else 'false'))
        w('inline constexpr int %s_key_moved = %d;' % (p, sc['held_delta'][1] - sc['held_delta'][0]))
        w('')
    w('struct Scenario {')
    w('    int capture; const char* title;')
    w('    const Placed* before; size_t nbefore; const Request* requests; size_t nrequests; const Event* events; size_t nevents;')
    w('    const Held* held; const Shown* shown; size_t nshown; bool has_appearance; int key_moved;')
    w('};')
    w('inline constexpr Scenario kScenarios[] = {')
    for sc in scenarios:
        p = 'k%d' % sc['cid']
        w('    {%d, "%s", %s_before, sizeof %s_before / sizeof(Placed), %s_requests, sizeof %s_requests / sizeof(Request), %s_events, sizeof %s_events / sizeof(Event),'
          % (sc['cid'], sc['title'], p, p, p, p, p, p))
        w('     %s_held, %s_shown, %s_nshown, %s_has_appearance, %s_key_moved},' % (p, p, p, p, p))
    w('};')
    w('')
    allrec = sorted(set().union(*[sc['records'] for sc in scenarios]))
    w('inline constexpr Record kRecords[] = {')
    for rec in allrec:
        slots, two, name = infos[rec]
        assert len(slots) <= 4
        s = ', '.join(str(x) for x in slots + [0] * (4 - len(slots)))
        w('    {%dll, "%s", %d, {%s}, %s},' % (rec, name, len(slots), s, 'true' if two else 'false'))
    w('};')
    w('')
    w('}  // namespace capture_equip')
    # the line ending the file already has is kept (the header is CRLF in the repository), as gen_all.py does
    old = open(out_path, 'rb').read() if os.path.exists(out_path) else b''
    crlf = old.count(b'\r\n') * 2 > old.count(b'\n') > 0
    text = ('\n'.join(out) + '\n').replace('\n', '\r\n' if crlf else '\n').encode('utf-8')
    if check:
        if old == text:
            print('%s: unchanged (%d bytes, %d scenarios, %d records)' % (out_path, len(text), len(scenarios), len(allrec)))
            return True
        print('%s: DIFFERS from the regenerated text (%d bytes on disk, %d regenerated)' % (out_path, len(old), len(text)))
        return False
    open(out_path, 'wb').write(text)
    print('wrote', out_path, len(scenarios), 'scenarios', len(allrec), 'records')
    return True


if __name__ == '__main__':
    args = sys.argv[1:]
    if '--cc' in args:
        i = args.index('--cc')
        CC = args[i + 1]
        del args[i:i + 2]
    check = '--check' in args
    if check:
        args.remove('--check')
    if not CC or not args:
        raise SystemExit('usage: python tools/capture/gen_equip.py OUT.h [content.db] [--cc DIR] [--check]   (or AOC_RESEARCH_CC=DIR; --check writes nothing and exits 1 on a change)')
    ok = main(args[0], args[1] if len(args) > 1 else os.path.join(REPO, 'dist', 'content', 'content.db'), check)
    sys.exit(0 if ok else 1)
