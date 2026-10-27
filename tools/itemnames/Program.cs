// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports.Internationalization;
using Mapc;

// itemnames read --table CONTAINERPATH [--table ...] [--out-dir DIR] [--paks DIR] [--usmap FILE] [--check]
//   Reads the string tables under the given container paths (for example AOC/Content/UI_Kit/Items.uasset) and writes DIR/item_names.tsv: one line per entry,
//     table  key  text
//   `table` is the asset's object path as the design data names it in a text that is a string-table entry (/Game/UI_Kit/Items.Items), `key` the entry's key and `text` the words the
//   player reads; a backslash, a tab and a line break in a text are written \\, \t and \n.  Lines are in the order of table, then key (ordinal), so the same game files give the same bytes.
//   With --check it writes nothing and exits 1 when DIR/item_names.tsv differs from a fresh read.
const string FileName = "item_names.tsv";
const string Header = "table\tkey\ttext";
string paks = Game.DefaultPaks, usmap = Game.DefaultUsmap, outDir = Path.Combine("dist", "content");
bool check = false;
string cmd = "help";
var tables = new List<string>();
try
{
    for (int i = 0; i < args.Length; i++)
    {
        string a = args[i];
        string Val() => i + 1 < args.Length ? args[++i] : throw new ArgumentException(a + " needs a value");
        switch (a)
        {
            case "--paks": paks = Val(); break;
            case "--usmap": usmap = Val(); break;
            case "--out-dir": outDir = Val(); break;
            case "--table": tables.Add(Val()); break;
            case "--check": check = true; break;
            default:
                if (a.StartsWith("--")) throw new ArgumentException("unknown option " + a);
                if (cmd == "help") cmd = a; else throw new ArgumentException("unexpected argument " + a);
                break;
        }
    }
    if (cmd != "read" || tables.Count == 0) throw new ArgumentException("usage: itemnames read --table CONTAINERPATH [--table ...] [--out-dir DIR] [--paks DIR] [--usmap FILE] [--check]");
}
catch (ArgumentException ex)
{
    Console.Error.WriteLine(ex.Message);
    return 2;
}

static string Escape(string s) => s.Replace("\\", "\\\\").Replace("\t", "\\t").Replace("\r", "\\r").Replace("\n", "\\n");

// the object path the design data gives a text of the table: "/Game/" + the path under AOC/Content without ".uasset", a dot and the asset's name
static string ObjectPath(string containerPath)
{
    const string root = "AOC/Content/";
    if (!containerPath.StartsWith(root, StringComparison.Ordinal) || !containerPath.EndsWith(".uasset", StringComparison.Ordinal))
        throw new ArgumentException(containerPath + " is not a path of the form AOC/Content/.../Name.uasset");
    string rel = containerPath.Substring(root.Length, containerPath.Length - root.Length - ".uasset".Length);
    return "/Game/" + rel + "." + rel.Substring(rel.LastIndexOf('/') + 1);
}

var game = new Game(paks, usmap);
var rows = new List<(string Table, string Key, string Text)>();
foreach (var path in tables.OrderBy(t => t, StringComparer.Ordinal))
{
    IPackage? pkg;
    try { pkg = game.Provider.LoadPackage(path); } catch (Exception ex) { Console.Error.WriteLine($"{path}: {ex.Message}"); return 1; }
    if (pkg == null) { Console.Error.WriteLine($"{path}: not in the game's containers"); return 1; }
    UStringTable? st = pkg.GetExports().OfType<UStringTable>().FirstOrDefault();
    if (st == null || st.StringTable == null) { Console.Error.WriteLine($"{path}: holds no string table"); return 1; }
    string table = ObjectPath(path);
    foreach (var (key, text) in st.StringTable.KeysToEntries) rows.Add((table, key, text));
    Console.WriteLine($"itemnames: {path}: {st.StringTable.KeysToEntries.Count} entries");
}
rows.Sort((a, b) => { int c = string.CompareOrdinal(a.Table, b.Table); return c != 0 ? c : string.CompareOrdinal(a.Key, b.Key); });
for (int i = 1; i < rows.Count; i++)
    if (rows[i].Table == rows[i - 1].Table && rows[i].Key == rows[i - 1].Key) { Console.Error.WriteLine($"{rows[i].Table}: the key {rows[i].Key} is there twice"); return 1; }
var sb = new StringBuilder();
sb.Append(Header).Append('\n');
foreach (var (table, key, text) in rows) sb.Append(table).Append('\t').Append(Escape(key)).Append('\t').Append(Escape(text)).Append('\n');
string output = sb.ToString();
string file = Path.Combine(outDir, FileName);
if (check)
{
    bool same = File.Exists(file) && File.ReadAllText(file, Encoding.UTF8) == output;
    Console.WriteLine($"{FileName}: {(same ? "same" : "DIFFERENT")}");
    return same ? 0 : 1;
}
Directory.CreateDirectory(outDir);
var tmp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
try { File.WriteAllText(tmp, output, new UTF8Encoding(false)); File.Move(tmp, file, true); }
finally { if (File.Exists(tmp)) File.Delete(tmp); }
Console.WriteLine($"wrote {file}: {rows.Count} entries of {tables.Count} table(s)");
return 0;
