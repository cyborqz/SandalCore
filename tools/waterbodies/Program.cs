// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;
using Mapc;
using WaterBodies;

// waterbodies read [--out-dir DIR] [--paks DIR] [--usmap FILE] [--region FILE] [--margin CM] [--check]
//   Reads the water body actors of the Water grid's cells that touch the Riverlands and writes DIR/fishing_water.tsv: the lakes and rivers a line can be cast into (the ones
//   that name fishing data), each with its outline and swim volumes.  With --check it writes nothing and exits 1 when DIR's file differs from a fresh read.
//   waterbodies list [--paks DIR] [--usmap FILE] [--region FILE] [--margin CM]    prints what the grid holds (every water body, the fishable ones marked) and writes nothing.
string paks = Game.DefaultPaks, usmap = Game.DefaultUsmap, outDir = Path.Combine("dist", "content");
string? regionPath = null;
double margin = Cli.GeometryMarginCm;
bool check = false;
var restArgs = new List<string>();
string cmd = "help";
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
            case "--region": regionPath = Val(); break;
            case "--margin": margin = double.Parse(Val(), System.Globalization.CultureInfo.InvariantCulture); break;
            case "--check": check = true; break;
            default:
                if (a.StartsWith("--")) throw new ArgumentException("unknown option " + a);
                if (cmd == "help") cmd = a; else restArgs.Add(a);
                break;
        }
    }
    if (cmd != "read" && cmd != "list" && cmd != "dump") throw new ArgumentException("usage: waterbodies read [--out-dir DIR] [--paks DIR] [--usmap FILE] [--region FILE] [--margin CM] [--check]\n       waterbodies list [--paks DIR] [--usmap FILE] [--region FILE] [--margin CM]");
}
catch (ArgumentException ex)
{
    Console.Error.WriteLine(ex.Message);
    return 2;
}

if (cmd == "dump")
{
    // waterbodies dump CELL NAME-PART [CHARS]: the properties of the exports of a cell whose name holds NAME-PART (diagnostic)
    var game0 = new Game(paks, usmap);
    return Water.Dump(game0, restArgs[0], restArgs[1], restArgs.Count > 2 ? int.Parse(restArgs[2]) : 3000);
}
var scope = new Scope(Region.Load(regionPath ?? Region.DefaultPath()), Cli.CenterX, Cli.CenterY, Cli.RadiusCm, margin);
var game = new Game(paks, usmap);
var bodies = Water.Collect(game, scope, out var stats);
Console.WriteLine($"waterbodies: {stats.Cells} Water cells ({stats.Unreadable} unreadable), {stats.Actors} water body actors ({stats.Lakes} lakes, {stats.Rivers} rivers), {bodies.Count} fishable ({bodies.Count(b => b.Class == "lake")} lakes, {bodies.Count(b => b.Class == "river")} rivers), {bodies.Sum(b => b.Boxes.Count)} swim volumes, {bodies.Count(b => b.Outline.Count > 0)} outlines");
foreach (var kv in stats.FishingData.OrderBy(k => k.Key)) Console.WriteLine($"  fishing data {kv.Key}: {kv.Value} water bodies");
if (cmd == "list")
{
    foreach (var b in bodies) Console.WriteLine($"  {b.Class,-18} {b.Actor} fishing {b.Fishing} at ({b.X:F0}, {b.Y:F0}, {b.Z:F0}) {b.Boxes.Count} volumes, outline {b.Outline.Count} points");
    return 0;
}
if (stats.Unreadable > 0) { Console.Error.WriteLine($"waterbodies: {stats.Unreadable} cell(s) could not be read: nothing written"); return 1; }
string text = Water.ToTsv(bodies);
string file = Path.Combine(outDir, Water.FileName);
if (check)
{
    bool same = File.Exists(file) && File.ReadAllText(file, Encoding.UTF8) == text;
    Console.WriteLine($"{Water.FileName}: {(same ? "same" : "DIFFERENT")}");
    return same ? 0 : 1;
}
Directory.CreateDirectory(outDir);
var tmp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
try { File.WriteAllText(tmp, text, new UTF8Encoding(false)); File.Move(tmp, file, true); }
finally { if (File.Exists(tmp)) File.Delete(tmp); }
Console.WriteLine($"wrote {file}");
return 0;
