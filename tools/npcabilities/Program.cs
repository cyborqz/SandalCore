// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;
using Mapc;
using NpcAbilities;

// npcabilities read [--dir CONTAINERFOLDER]... [--out-dir DIR] [--paks DIR] [--usmap FILE] [--check]
//   Reads the creature Blueprints and the animation montages under the given container folders (for example AOC/Content/DesignAssets/Population/NPC/Beast/Rabid)
//   and writes DIR/npc_abilities.tsv and DIR/npc_montages.tsv.  With --check it writes nothing and exits 1 when DIR's files differ from a fresh read.
string paks = Game.DefaultPaks, usmap = Game.DefaultUsmap, outDir = Path.Combine("dist", "content");
bool check = false, any = false;
string cmd = "help";
var dirs = new List<string>();
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
            case "--dir": dirs.Add(Val()); break;
            case "--check": check = true; break;
            default:
                if (a.StartsWith("--")) throw new ArgumentException("unknown option " + a);
                if (cmd == "help") cmd = a; else throw new ArgumentException("unexpected argument " + a);
                break;
        }
    }
    if (cmd != "read" || dirs.Count == 0) throw new ArgumentException("usage: npcabilities read --dir CONTAINERFOLDER [--dir ...] [--out-dir DIR] [--paks DIR] [--usmap FILE] [--check]");
    any = true;
}
catch (ArgumentException ex)
{
    Console.Error.WriteLine(ex.Message);
    return 2;
}

var game = new Game(paks, usmap);
Reader.Collect(game, dirs, out var abilities, out var montages, out int packages, out int failed);
if (failed > 0) { Console.Error.WriteLine($"{failed} of {packages} packages could not be read"); return 1; }
string abilitiesText = Reader.AbilitiesTsv(abilities), montagesText = Reader.MontagesTsv(montages);
Console.WriteLine($"npcabilities: {packages} packages, {abilities.Count} ability tasks of {abilities.Select(a => a.Archetype).Distinct().Count()} Blueprints, {montages.Count} montages");
var files = new (string Name, string Text)[] { (Reader.AbilitiesFile, abilitiesText), (Reader.MontagesFile, montagesText) };
if (check)
{
    int different = 0;
    foreach (var (name, text) in files)
    {
        var path = Path.Combine(outDir, name);
        bool same = File.Exists(path) && File.ReadAllText(path, Encoding.UTF8) == text;
        Console.WriteLine($"{name}: {(same ? "same" : "DIFFERENT")}");
        if (!same) different++;
    }
    return different == 0 ? 0 : 1;
}
Directory.CreateDirectory(outDir);
foreach (var (name, text) in files)
{
    var path = Path.Combine(outDir, name);
    var tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
    try { File.WriteAllText(tmp, text, new UTF8Encoding(false)); File.Move(tmp, path, true); }
    finally { if (File.Exists(tmp)) File.Delete(tmp); }
    Console.WriteLine($"wrote {path}");
}
return any ? 0 : 2;
