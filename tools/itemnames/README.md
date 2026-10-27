# itemnames

Reads the words the game's items are named with out of your own game files.

    dotnet tools/itemnames/bin/Release/net8.0/itemnames.dll read --table AOC/Content/UI_Kit/Items.uasset [--table ...] [--out-dir dist/content] [--check]

`scripts/extract_game_data.ps1` runs it as the `names` step (`-Only tools,names`); it needs only the game's containers (a few seconds).
The tables it reads are listed in the script (`$StringTables`): the string tables that an item's name points at in the design data.

It writes `item_names.tsv`, one line per entry of each table: `table` (the table's object path, as the design data names it, for example `/Game/UI_Kit/Items.Items`), `key`
and `text` (a backslash, a tab and a line break in a text are written `\\`, `\t` and `\n`). The lines are in the order of table and key, so the same game files give the same bytes.
`contentc` reads the file (`--item-names`, default `dist/content/item_names.tsv`) and writes the text, instead of the key, into the item records whose name is a string-table entry
(the six Glint kinds and six quest items): the pick-up pop-up then reads "Dim Glint" and not a text code.

`--check` writes nothing and exits 1 when `--out-dir` holds a different file.
