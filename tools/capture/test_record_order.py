# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
"""The generators that read content.db's record table by schema read the rows in the order the table was written in (rowid).

contentc makes an index on (schema, guid) after the last row (services/cmd/contentc/record_index.go). A query `WHERE schema = ?` that names no order then comes back in guid order,
and in rowid order from a file that has no such index. gen_gm_garden, gen_tooltips (the first guid of a name wins) and gen_town_layouts say `ORDER BY rowid`, so that what they write
does not depend on which of the two files they read. This makes a small database with the index, guids in the opposite order of the rows, and checks each.

    python -I tools/capture/test_record_order.py        exit 0 when every check holds, 1 otherwise
"""
import gzip
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_gm_garden  # noqa: E402
import gen_tooltips  # noqa: E402
import gen_town_layouts  # noqa: E402

checks = 0
failed = 0


def check(ok, what):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print('FAIL:', what)
    else:
        print('ok:  ', what)


def make_db(path, rows):
    """rows: (schema, guid, name) in the order they are written. The two indexes are the ones contentc makes."""
    con = sqlite3.connect(path)
    con.execute('CREATE TABLE record (table_index INTEGER NOT NULL, type_id INTEGER NOT NULL, schema TEXT NOT NULL, guid INTEGER NOT NULL, parent_guid INTEGER NOT NULL, '
                'name TEXT NOT NULL, dev_only INTEGER NOT NULL, data BLOB NOT NULL, UNIQUE (type_id, guid))')
    blob = gzip.compress(b'{}')
    for i, (schema, guid, name) in enumerate(rows):
        con.execute('INSERT INTO record VALUES (?, ?, ?, ?, 0, ?, 0, ?)', (0, 1000 + i, schema, guid, name, blob))
    con.execute('CREATE INDEX record_by_guid ON record (guid)')
    con.execute('CREATE INDEX record_by_schema ON record (schema, guid)')
    con.commit()
    plan = ' '.join(r[3] for r in con.execute("EXPLAIN QUERY PLAN SELECT guid FROM record WHERE schema = 'x'"))
    con.close()
    return plan


with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, 'content.db')
    garden = 'GatherableActorDefRecordBase'
    ability = 'AoCAbilityRecordBase'
    layout = gen_town_layouts.SCHEMAS[0]
    # the guids run down while the rows are written: a read in guid order is the reverse of a read in rowid order
    rows = [(garden, 30, 'G30'), (garden, 20, 'G20'), (garden, 10, 'G10'),
            (ability, 70, 'Dup'), (ability, 60, 'Other'), (ability, 50, 'Dup'),
            (layout, 300, 'L300'), (layout, 200, 'L200'), (layout, 100, 'L100')]
    plan = make_db(path, rows)
    check('record_by_schema' in plan, 'the test database reads a schema through record_by_schema (%s)' % plan)

    db = sqlite3.connect('file:' + path.replace(chr(92), '/') + '?mode=ro', uri=True)
    check(list(gen_gm_garden.load(db, garden)) == [30, 20, 10], 'gen_gm_garden reads a schema in rowid order, not in guid order')
    db.close()

    tooltips = gen_tooltips.DB(path)
    names = tooltips.names.get(ability, {})
    tooltips.c.close()
    check(names.get('Dup') == 70 and names.get('dup') == 70, 'gen_tooltips: of two records with one name the first written wins (%s)' % names.get('Dup'))

    layouts = gen_town_layouts.Records(path)
    blobs = list(layouts.blobs)
    layouts.db.close()
    check(blobs == [300, 200, 100], 'gen_town_layouts reads a schema in rowid order, not in guid order (%s)' % blobs)

print('%d checks, %d failed' % (checks, failed))
sys.exit(1 if failed else 0)
