# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""The rows of content.db that the navigation mesh step reads, as text.

When mapc build runs aoc_navbuild with --content, aoc_navbuild stands the five towns' floors at their node records: x, y, z and yaw of city_node. Nothing else of content.db is read
by the step. extract_game_data.ps1 hashes this text into the step's id, so that the step (the longest one) runs again when a town node moves and not when other game numbers
(abilities, items, quests) change.

    python scripts/nav_inputs.py dist/content/content.db

One line per node, in guid order: guid x y z yaw. Every node is listed, not only the five towns', so that a change of which nodes aoc_navbuild reads needs no change here. A float
is printed in the shortest form that reads back as the same number. Exit code 0 with the lines on stdout; 1 with a message on stderr when the table cannot be read.
"""
import pathlib
import sqlite3
import sys


def main(argv):
    if len(argv) != 2:
        sys.stderr.write('usage: nav_inputs.py CONTENT_DB\n')
        return 2
    path = pathlib.Path(argv[1])
    if not path.is_file():
        sys.stderr.write('nav_inputs: %s is missing\n' % path)
        return 1
    try:
        con = sqlite3.connect('file:' + str(path).replace('\\', '/') + '?mode=ro', uri=True)
        try:
            rows = con.execute('SELECT guid, x, y, z, yaw FROM city_node ORDER BY guid').fetchall()
        finally:
            con.close()
    except sqlite3.Error as e:
        sys.stderr.write('nav_inputs: %s: city_node cannot be read (%s)\n' % (path, e))
        return 1
    for row in rows:
        sys.stdout.write(' '.join(repr(v) for v in row) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
