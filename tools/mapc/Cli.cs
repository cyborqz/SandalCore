// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace Mapc;

/// <summary>
/// mapc: the area's ground, offline.  The area is the Riverlands' rectangles (tools/mapc/region_rects.tsv; G1 stage 1) unless --center/--radius name E1's circle.
///   mapc probe-cells [--batches M]   the master umap alone: the cells the area reads, per grid, and how M batches would cut them
///   mapc extract [--out-dir DIR]     read the cooked map for the area (a region: in --batches M child processes under a heap limit), write DIR/area_geom.bin
///                                    (and, for a region, DIR/region_foliage.tsv: the gatherable foliage)
///   mapc build   [--out-dir DIR]     extract, run aoc_navbuild (DIR/area_nav.bin) and read the NPC Blueprints (DIR/npc_motion.tsv); when DIR has town_platforms.bin and content.db
///                                    aoc_navbuild also stands the five towns' platforms at their node records (the navigation mesh then has the towns' own floors)
///   mapc --check [--out-dir DIR]     rebuild all three in a scratch folder and compare them byte for byte with DIR's; exit 1 on a difference
///                                    (`mapc build --check` is the same; `--check` stands anywhere: `mapc --check roads` is `mapc roads --check`; `--explain` prints what a line means and stops)
///   mapc export-config               the client's Config directory (AOC/Config: DefaultCombat.ini, DefaultAoCRecordConstants.ini ...) as the game ships it, into DIR/client_config
///   mapc export-zoi                  the zone-of-influence chunk packages of layer 0 (C_x_y_0.uasset) as the game ships them, into DIR/zoi_chunks (no map read: seconds)
///   mapc mount-speeds [--check]      read the 141 mount Blueprints' MaxWalkSpeed, MaxFlySpeed and MaxSwimSpeed (no map: seconds), write DIR/mount_speeds.tsv;
///                                    with --check write nothing and exit 1 when DIR's file differs from a fresh read (mounts milestone)
///   mapc artisan-curves [--check]    read the three profession level-up curves (Gathering, Processing, Crafting: pak assets the design database does not hold; no map:
///                                    seconds), write DIR/artisan_curves.tsv; with --check write nothing and exit 1 when DIR's file differs from a fresh read (Gathering stage 2)
///   mapc roads [--check]             read the BP_RoadActor_C splines of the area's MainGrid cells, write DIR/roads_raw.tsv (a map read: run it alone; mounts milestone, stage 4)
///   mapc town-floors [--check]       read the two city-node platform umaps (Landscape_Flat_D_Master_01, Landscape_River_C_Master_01: 48 MB, no map read), write DIR/town_platforms.bin
///                                    (the heights and holes of the five bowls' floor); with --check write nothing and exit 1 when DIR's file differs
///   mapc town-layouts [--check]      read the static meshes and level instances that the five towns' layouts place (dist/content/town_layout_items.tsv, made by
///                                    tools/capture/gen_town_layouts.py) from the game's containers, write DIR/town_layouts.bin (their collision geometry, in area_geom.bin's format;
///                                    `mapc build` merges it into area_geom.bin); with --check write nothing and exit 1 when DIR's file differs.  --only NAME, --levels HEX, --probe PATH...
///   mapc merge-geometry BASE LAYER OUT [--box X0,Y0,X1,Y1]   two geometry files as one (a scratch measurement: the box replaces the footprint, so that aoc_navbuild builds only its tiles)
///   mapc probe KEY.tsv               the answer key's hit rate on the extracted geometry (diagnostic)
///   mapc inspect NAME...             what one static mesh holds (diagnostic)
/// Options: --paks DIR, --usmap FILE, --navbuild EXE, --center X,Y, --radius CM, --margin CM, --threads N.
/// </summary>
public static class Cli
{
    // the area: the login spot and 1 km (server/worldd/spawns/area.h holds the same numbers; the tests compare)
    public const double CenterX = -777664.5, CenterY = 616578.6875, RadiusCm = 100000.0;
    public const double GeometryMarginCm = 15000.0;
// CoA / C Y B

    sealed class Opt
    {
        public string Cmd = "help";
        public string OutDir = Path.Combine("dist", "content");
        public string Paks = Game.DefaultPaks, Usmap = Game.DefaultUsmap;
        public string Navbuild = Path.Combine("dist", "Release", "aoc_navbuild.exe");
        public double Cx = CenterX, Cy = CenterY, Radius = RadiusCm, Margin = GeometryMarginCm;
        public bool Circle;                // --center / --radius given: E1's circle, not the Riverlands' rectangles
        public bool OutDirGiven;           // --out-dir was on the line (a circle must name its own: the default is the region's)
        public string? RegionPath;         // --region FILE (default tools/mapc/region_rects.tsv)
        public int Batches = 0;            // --batches M: the map read runs as M child processes (0: one process, in this one)
        public int BatchIndex = -1, BatchCount = 0;   // --cells N/M: this process reads batch N of M (a child of --batches)
        public string? Part;               // --part FILE: where a batch writes its geometry
        public double HeapGb = 0;          // --heap-gb G: DOTNET_GCHeapHardLimit of each child (0: the default below)
        public double MinFreeGb = 6;       // --min-free-gb G: a batch does not start with less memory free than this
        public double WaitMinutes = 40;    // --wait-minutes N: how long a batch waits for room (the game or a build running, too little memory free) before giving up
        public string? WorkDir;            // --work-dir DIR: where the batches' parts are kept (default: a folder beside the output; a finished part is reused by the next run)
        public int StopAfter = -1;         // --stop-after N: run batches 0..N, print their time and peak memory, merge nothing (the measurement run)
        public bool KeepParts;             // --keep-parts: do not delete the parts after a successful merge
        public int Threads = 0;
        public bool CheckFlag;       // --check anywhere on the line: the command compares instead of writing (resolved against the command by Parse)
        public bool Explain;         // --explain: print what the line means and stop (no work): the argument handling's test
        public string? Items;        // --items FILE: the town layouts' list (default DIR/town_layout_items.tsv)
        public string? Only;         // --only TEXT: the bowls whose name contains it
        public string? Levels;       // --levels HEX: the node levels (a bit each) the layouts are read for (default all seven)
        public string? Box;          // --box X0,Y0,X1,Y1: merge-geometry's footprint
        public List<string> Rest = new();
    }

    static Opt Parse(string[] args)
    {
        var o = new Opt();
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            string Val() => i + 1 < args.Length ? args[++i] : throw new ArgumentException(a + " needs a value");
            switch (a)
            {
                case "--check": o.CheckFlag = true; break;
                case "--explain": o.Explain = true; break;
                case "--probe": o.Rest.Add(a); break;
                case "--out-dir": o.OutDir = Val(); o.OutDirGiven = true; break;
                case "--paks": o.Paks = Val(); break;
                case "--usmap": o.Usmap = Val(); break;
                case "--navbuild": o.Navbuild = Val(); break;
                case "--center": { var p = Val().Split(','); o.Cx = double.Parse(p[0]); o.Cy = double.Parse(p[1]); o.Circle = true; break; }
                case "--radius": o.Radius = double.Parse(Val()); o.Circle = true; break;
                case "--region": o.RegionPath = Val(); break;
                case "--batches": o.Batches = int.Parse(Val()); break;
                case "--cells": { var p = Val().Split('/'); o.BatchIndex = int.Parse(p[0]); o.BatchCount = int.Parse(p[1]); if (o.BatchCount < 1 || o.BatchIndex < 0 || o.BatchIndex >= o.BatchCount) throw new ArgumentException("--cells N/M needs 0 <= N < M"); break; }
                case "--part": o.Part = Val(); break;
                case "--heap-gb": o.HeapGb = double.Parse(Val()); break;
                case "--min-free-gb": o.MinFreeGb = double.Parse(Val()); break;
                case "--wait-minutes": o.WaitMinutes = double.Parse(Val()); break;
                case "--work-dir": o.WorkDir = Val(); break;
                case "--keep-parts": o.KeepParts = true; break;
                case "--stop-after": o.StopAfter = int.Parse(Val()); break;
                case "--margin": o.Margin = double.Parse(Val()); break;
                case "--threads": o.Threads = int.Parse(Val()); break;
                case "--items": o.Items = Val(); break;
                case "--only": o.Only = Val(); break;
                case "--levels": o.Levels = Val(); break;
                case "--box": o.Box = Val(); break;
                default:
                    if (a.StartsWith("--")) throw new ArgumentException("unknown option " + a);
                    if (o.Cmd == "help") o.Cmd = a; else o.Rest.Add(a);
                    break;
            }
        }
        // review B I2: `--check` means the same wherever it stands on the line.  It used to switch the command to the E1 check only when it came first, so `mapc build --check`
        // rebuilt and OVERWROTE the outputs, and `mapc --check roads` started the whole E1 check (above 14 GB) and dropped `roads`.
        if (o.CheckFlag)
        {
            switch (o.Cmd)
            {
                case "help": case "build": case "extract": o.Cmd = "check"; break;   // the E1 gate: rebuild all three in a scratch folder and compare
                case "check": case "mount-speeds": case "roads": case "artisan-curves": case "town-floors": case "town-layouts": break;   // these compare themselves
                default: throw new ArgumentException("--check goes with build, mount-speeds, artisan-curves, roads, town-floors or town-layouts (or alone: the E1 check), not with " + o.Cmd);
            }
        }
        // review B I7: E1's circle is not the default area any more; its build must not land in (or be compared with) the Riverlands' files in dist/content
        if (o.Circle && o.Cmd == "check")
            throw new ArgumentException("--check with --center/--radius is gone: E1's files were made by an earlier mapc (geometry v1, mesh names without @hash, 7 tiles differ since the package-path fix) and cannot be reproduced by this one byte for byte; E1's guarantee is test_worldd_nav_window's tile comparison (tools/mapc/README.md)");
        if (o.Circle && !o.OutDirGiven && (o.Cmd == "extract" || o.Cmd == "build"))
            throw new ArgumentException("--center/--radius (E1's circle) needs an explicit --out-dir: the default dist/content holds the Riverlands' files, which a circle build would overwrite (E1's own are in dist/content/e1)");
        return o;
    }
// -- Cyb & C | O | A --

    public static int Run(string[] args)
    {
        Opt o;
        try { o = Parse(args); } catch (ArgumentException e) { Console.Error.WriteLine("mapc: " + e.Message); return 2; }
        if (o.Explain) { Console.WriteLine($"command={o.Cmd} check={(o.CheckFlag ? "yes" : "no")} out-dir={o.OutDir}"); return 0; }
        try
        {
            switch (o.Cmd)
            {
                case "extract":
                {
                    if (o.BatchIndex >= 0) return ExtractChild(o);
                    Directory.CreateDirectory(o.OutDir);
                    ExtractAndWrite(o, o.OutDir);
                    return 0;
                }
                case "build": return Build(o, o.OutDir);
                case "check": return Check(o);
                case "probe": { var g = Extract(o, o.OutDir); Probe.Evaluate(g, o.Rest[0]); return 0; }
                case "uses": { var game = new Game(o.Paks, o.Usmap); Inspect.Uses(game, o.Rest[0], 12000, o.Rest.Count > 1 ? int.Parse(o.Rest[1]) : 3); return 0; }
                case "npc-motion": { var game = new Game(o.Paks, o.Usmap); var rows = NpcBlueprints.Collect(game, NpcBlueprints.AllPaths(game), out var n, out var bad); Directory.CreateDirectory(o.OutDir); File.WriteAllText(Path.Combine(o.OutDir, "npc_motion.tsv"), NpcBlueprints.ToTsv(rows)); Console.WriteLine($"{rows.Count} pawn Blueprints of {n} packages ({bad} unreadable)"); return 0; }
                case "npc-bp":
                {
                    // npc-bp OUT.tsv [PATHS.txt]: the pawn Blueprints (a list of package paths, or every package under DesignAssets)
                    var game = new Game(o.Paks, o.Usmap);
                    IEnumerable<string> paths = o.Rest.Count > 1
                        ? File.ReadAllLines(o.Rest[1]).Where(l => l.Length > 0)
                        : NpcBlueprints.AllPaths(game);
                    var rows = NpcBlueprints.Collect(game, paths, out var n, out var bad);
                    File.WriteAllText(o.Rest[0], NpcBlueprints.ToTsv(rows));
                    Console.WriteLine($"{rows.Count} pawn Blueprints of {n} packages ({bad} unreadable)");
                    return 0;
                }
                case "probe-cells":
                {
                    var game = new Game(o.Paks, o.Usmap);
                    return Diag.ProbeCells(game, MakeScope(o), o.Batches, m => Console.WriteLine(m));
                }
                case "foliage-probe": { var game = new Game(o.Paks, o.Usmap); return Diag.FoliageProbe(game, MakeScope(o), o.Rest.Count > 0 ? int.Parse(o.Rest[0]) : 4); }
                case "foliage-detail": { var game = new Game(o.Paks, o.Usmap); return Diag.FoliageDetail(game, o.Rest[0], o.Rest.Count > 1 ? int.Parse(o.Rest[1]) : 3); }
                case "cell-exports": { var game = new Game(o.Paks, o.Usmap); return Diag.CellExports(game, o.Rest[0], o.Rest.Count > 1 ? o.Rest[1] : null, 12); }
                case "export-config": return Export.Config(new Game(o.Paks, o.Usmap), o.OutDir);
                case "export-zoi": return Export.Zoi(new Game(o.Paks, o.Usmap), o.OutDir);
                case "mount-speeds": return MountSpeedsCommand(o);
                case "artisan-curves": return ArtisanCurvesCommand(o);
                case "roads": return RoadsCommand(o);
                case "town-floors": return TownFloorsCommand(o);
                case "town-layouts": return TownLayoutsCommand(o);
                case "merge-geometry": return MergeGeometryCommand(o);
                case "bp": { var game = new Game(o.Paks, o.Usmap); Inspect.Bp(game, o.Rest[0], o.Rest.Count > 1 ? o.Rest[1] : null); return 0; }
                case "inspect": { var game = new Game(o.Paks, o.Usmap); Inspect.Run(game, new[] { "inspect" }.Concat(o.Rest).ToArray()); return 0; }
                default:
                    Console.WriteLine("mapc extract | build | --check | probe-cells | export-config | export-zoi | mount-speeds [--check] | artisan-curves [--check] | roads [--check] | town-floors [--check] | town-layouts [--check] | merge-geometry BASE LAYER OUT | probe KEY.tsv | inspect NAME...   [--out-dir D --paks D --usmap F --navbuild EXE --region FILE | --center X,Y --radius CM  --margin CM --threads N --batches M --heap-gb G --min-free-gb G --work-dir D --stop-after N]");
                    return o.Cmd == "help" ? 0 : 2;
            }
        }
        catch (StoppedAfterBatch s)
        {
            Console.WriteLine("mapc: " + s.Message);
            foreach (var st in s.Stats) Console.WriteLine("  " + st);
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("mapc: " + e.Message);
            return 1;
        }
    }

    /// <summary>mapc roads: the road splines of the area's MainGrid cells into DIR/roads_raw.tsv (a map read: run it alone), or with --check a byte comparison with the file
    /// that is there.  `mapc roads --probe CELLID` shows what one cell's road actors hold.</summary>
    static int RoadsCommand(Opt o)
    {
        var game = new Game(o.Paks, o.Usmap);
        if (o.Rest.Count >= 2 && o.Rest[0] == "--probe") { Roads.Probe(game, o.Rest[1]); return 0; }
        var scope = MakeScope(o);
        var res = Roads.Collect(game, scope, m => Console.WriteLine(m));
        double km = res.Roads.Sum(r => r.LengthCm) / 100000.0;
        Console.WriteLine($"roads: {res.CellsRead} cells read ({res.CellsMissing} named by the map and not in the containers), {res.CellsWithRoads} hold road actors, {res.ActorsSeen} actors seen, {res.ActorsDecoded} decoded, {res.Duplicates} duplicates (a road in two cells), {res.Roads.Count} roads, {km:F1} km, {res.Roads.Sum(r => r.Poly.Count)} samples");
        Console.WriteLine($"roads: {res.Seconds:F0} s, peak working set {res.PeakWorkingSetBytes / (1024.0 * 1024 * 1024):F2} GB");
        foreach (var w in res.Warnings.Take(20)) Console.WriteLine("roads: warning: " + w);
        string text = Roads.ToTsv(res, scope);
        string file = Path.Combine(o.OutDir, Roads.RawFileName);
        if (o.CheckFlag)
        {
            if (!File.Exists(file)) { Console.WriteLine($"{Roads.RawFileName}: missing in {o.OutDir}"); return 1; }
            var have = File.ReadAllBytes(file); var fresh = Encoding.UTF8.GetBytes(text);
            bool same = have.AsSpan().SequenceEqual(fresh);
            Console.WriteLine(same ? $"{Roads.RawFileName}: identical ({have.Length} bytes)" : $"{Roads.RawFileName}: DIFFERS (file {have.Length} bytes, fresh {fresh.Length})");
            return same && res.Warnings.Count == 0 ? 0 : 1;
        }
        Directory.CreateDirectory(o.OutDir);
        File.WriteAllText(file, text);
        Console.WriteLine($"wrote {file}");
        return res.Warnings.Count == 0 ? 0 : 1;
    }
// Cyb : C.O.A

    /// <summary>mapc town-floors: the two city-node platform umaps (Landscape_Flat_D_Master_01, Landscape_River_C_Master_01: 48 MB, not a map read) into DIR/town_platforms.bin, or with
    /// --check a byte comparison with the file that is there.  It runs as a child under a heap limit (--heap-gb, 3 by default) behind the same guard as the map read (free memory, no game
    /// or build running); `--probe` lists what the two umaps hold and writes nothing.</summary>
    static int TownFloorsCommand(Opt o)
    {
        void Log(string m) => Console.WriteLine(m);
        if (o.Rest.Contains("--probe")) return TownFloors.Probe(new Game(o.Paks, o.Usmap), Log);
        if (Environment.GetEnvironmentVariable("DOTNET_GCHeapHardLimit") == null)
        {
            // the guard, then this same command line again as a child under DOTNET_GCHeapHardLimit
            if (!Sys.WaitForRoom(o.MinFreeGb, o.WaitMinutes, Log)) { Console.Error.WriteLine("mapc: no room on this machine"); return 1; }
            var psi = new ProcessStartInfo(Environment.ProcessPath ?? "dotnet") { UseShellExecute = false };
            psi.ArgumentList.Add(typeof(Cli).Assembly.Location);
            foreach (var a in Environment.GetCommandLineArgs().Skip(1)) psi.ArgumentList.Add(a);
            double heapGb = o.HeapGb > 0 ? o.HeapGb : 3;
            psi.Environment["DOTNET_GCHeapHardLimit"] = ((ulong)(heapGb * 1073741824.0)).ToString("X", CultureInfo.InvariantCulture);
            psi.Environment["DOTNET_gcConcurrent"] = "0";
            Log($"town-floors: running under a heap limit of {heapGb:F1} GB ({Sys.FreePhysicalGb():F1} GB free)");
            using var child = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + psi.FileName);
            child.WaitForExit();
            return child.ExitCode;
        }
        var sw = Stopwatch.StartNew();
        var game = new Game(o.Paks, o.Usmap);
        var bytes = TownFloors.Build(game, m => Console.WriteLine(m), out int notes);
        var proc = Process.GetCurrentProcess();
        Log($"town-floors: {sw.Elapsed.TotalSeconds:F1} s, peak working set {proc.PeakWorkingSet64 / 1073741824.0:F2} GB, GC heap {GC.GetGCMemoryInfo().HeapSizeBytes / 1073741824.0:F2} GB");
        string file = Path.Combine(o.OutDir, TownFloors.FileName);
        if (o.CheckFlag)
        {
            if (!File.Exists(file)) { Log($"{TownFloors.FileName}: missing in {o.OutDir}"); return 1; }
            var have = File.ReadAllBytes(file);
            bool same = have.AsSpan().SequenceEqual(bytes);
            Log(same ? $"{TownFloors.FileName}: identical ({have.Length} bytes)" : $"{TownFloors.FileName}: DIFFERS (file {have.Length} bytes, fresh {bytes.Length})");
            return same ? 0 : 1;
        }
        Directory.CreateDirectory(o.OutDir);
        string tmp = file + ".tmp";
        File.WriteAllBytes(tmp, bytes);
        File.Move(tmp, file, true);
        Log($"wrote {file} ({bytes.Length} bytes)");
        return 0;
    }

    /// <summary>mapc town-layouts: the collision geometry of what the five towns' layouts place (TownLayouts.cs) into DIR/town_layouts.bin, or with --check a byte comparison with the file
    /// that is there.  A package read: it runs as a child under a heap limit (--heap-gb, 4 by default) behind the map read's guard (--min-free-gb free memory, no game or build running).
    /// `--probe PATH...` lists what the named level instances hold and writes nothing.</summary>
    static int TownLayoutsCommand(Opt o)
    {
        void Log(string m) => Console.WriteLine(m);
        if (o.Rest.Contains("--probe")) return TownLayouts.Probe(new Game(o.Paks, o.Usmap), o.Rest.Where(a => a != "--probe"), Log);
        string itemsFile = o.Items ?? Path.Combine(o.OutDir, TownLayouts.ItemsFileName);
        if (!File.Exists(itemsFile)) { Console.Error.WriteLine($"mapc: {itemsFile} is missing (python tools/capture/gen_town_layouts.py writes it)"); return 1; }
        if (Environment.GetEnvironmentVariable("DOTNET_GCHeapHardLimit") == null)
        {
            if (!Sys.WaitForRoom(o.MinFreeGb, o.WaitMinutes, Log)) { Console.Error.WriteLine("mapc: no room on this machine"); return 1; }
            var psi = new ProcessStartInfo(Environment.ProcessPath ?? "dotnet") { UseShellExecute = false };
            psi.ArgumentList.Add(typeof(Cli).Assembly.Location);
            foreach (var a in Environment.GetCommandLineArgs().Skip(1)) psi.ArgumentList.Add(a);
            double heapGb = o.HeapGb > 0 ? o.HeapGb : 4;
            psi.Environment["DOTNET_GCHeapHardLimit"] = ((ulong)(heapGb * 1073741824.0)).ToString("X", CultureInfo.InvariantCulture);
            psi.Environment["DOTNET_gcConcurrent"] = "0";
            Log($"town-layouts: running under a heap limit of {heapGb:F1} GB ({Sys.FreePhysicalGb():F1} GB free)");
            using var child = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + psi.FileName);
            child.WaitForExit();
            return child.ExitCode;
        }
        var sw = Stopwatch.StartNew();
        int levelMask = o.Levels == null ? TownLayouts.AllLevels : int.Parse(o.Levels, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        var items = TownLayouts.Parse(File.ReadLines(itemsFile), itemsFile);
        HashSet<long>? nodes = null;
        if (o.Only != null)
        {
            nodes = items.Bowls.Where(b => b.Name.Contains(o.Only, StringComparison.OrdinalIgnoreCase)).Select(b => b.Node).ToHashSet();
            if (nodes.Count == 0) { Console.Error.WriteLine($"mapc: no bowl of {itemsFile} has '{o.Only}' in its name"); return 1; }
        }
        Log($"town-layouts: {items.Bowls.Count} bowls, {items.Assets.Count} assets ({items.Assets.Count(a => a.Kind == 'L')} level instances), {items.Rows.Count} rows; levels {levelMask:X}" + (nodes != null ? $"; only {string.Join(", ", items.Bowls.Where(b => nodes.Contains(b.Node)).Select(b => b.Name))}" : ""));
        var game = new Game(o.Paks, o.Usmap);
        var g = new AreaGeometry();
        var src = new TownLayouts.GameSource(game, g, Log);
        TownLayouts.Compose(items, src, levelMask, nodes, g);
        var bytes = GeomFile.Write(g);
        var proc = Process.GetCurrentProcess();
        foreach (var kv in g.Stats) Log($"  {kv.Key} = {kv.Value}");
        Log($"town-layouts: {TownLayouts.Describe(g)}; {src.Packages} packages read, {g.Warnings.Count} warnings; {bytes.Length} bytes in {sw.Elapsed.TotalSeconds:F1} s, peak working set {proc.PeakWorkingSet64 / 1073741824.0:F2} GB, GC heap {GC.GetGCMemoryInfo().HeapSizeBytes / 1073741824.0:F2} GB");
        string file = Path.Combine(o.OutDir, TownLayouts.FileName);
        if (o.CheckFlag)
        {
            if (!File.Exists(file)) { Log($"{TownLayouts.FileName}: missing in {o.OutDir}"); return 1; }
            var have = File.ReadAllBytes(file);
            bool same = have.AsSpan().SequenceEqual(bytes);
            Log(same ? $"{TownLayouts.FileName}: identical ({have.Length} bytes)" : $"{TownLayouts.FileName}: DIFFERS (file {have.Length} bytes, fresh {bytes.Length})");
            return same ? 0 : 1;
        }
        Directory.CreateDirectory(o.OutDir);
        string tmp = file + ".tmp";
        File.WriteAllBytes(tmp, bytes);
        File.Move(tmp, file, true);
        Log($"wrote {file} ({bytes.Length} bytes)");
        return 0;
    }

    /// <summary>mapc merge-geometry BASE LAYER OUT: two geometry files as one, the way `mapc build` merges the towns' layers into the map's (a scratch measurement).  --box replaces the footprint
    /// by one rectangle, so that aoc_navbuild builds only the tiles that touch it.</summary>
    static int MergeGeometryCommand(Opt o)
    {
        if (o.Rest.Count != 3) { Console.Error.WriteLine("mapc: merge-geometry BASE.bin LAYER.bin OUT.bin [--box X0,Y0,X1,Y1]"); return 2; }
        var baseGeometry = GeomFile.Read(File.ReadAllBytes(o.Rest[0]));
        var layer = TownLayouts.ReadLayer(o.Rest[1]);
        var g = GeomFile.Merge(new[] { baseGeometry, layer });
        baseGeometry = null; layer = null;
        if (o.Box != null)
        {
            var p = o.Box.Split(',').Select(s => double.Parse(s, CultureInfo.InvariantCulture)).ToArray();
            if (p.Length != 4 || !(p[2] > p[0] && p[3] > p[1])) { Console.Error.WriteLine("mapc: --box X0,Y0,X1,Y1"); return 2; }
            var reg = new Region { Source = "box " + o.Box, Hash = 0xB0B0B0B0B0B0B0B0ul };
            reg.Rects.Add((p[0], p[1], p[2], p[3]));
            reg.MinX = p[0]; reg.MinY = p[1]; reg.MaxX = p[2]; reg.MaxY = p[3];
            g.Region = reg;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(o.Rest[2]))!);
        File.WriteAllBytes(o.Rest[2], GeomFile.Write(g));
        Console.WriteLine($"merge-geometry: {g.Tiles.Count} terrain tiles, {TownLayouts.Describe(g)} -> {o.Rest[2]} ({new FileInfo(o.Rest[2]).Length} bytes)");
        return 0;
    }

    /// <summary>mapc mount-speeds: the speeds of the mount Blueprints into DIR/mount_speeds.tsv, or with --check a byte comparison with the file that is there.</summary>
    static int MountSpeedsCommand(Opt o)
    {
        var game = new Game(o.Paks, o.Usmap);
        var rows = MountSpeeds.Collect(game, MountSpeeds.AllPaths(game), out var packages, out var bad);
        var outputs = new[] { (MountSpeeds.FileName, MountSpeeds.ToTsv(rows)), (MountSpeeds.RidersFileName, MountSpeeds.ToRidersTsv(rows)) };
        Console.WriteLine($"{rows.Count} mount Blueprints of {packages} packages ({bad} unreadable), {rows.Count(r => r.HasMovement)} with a movement component, {rows.Count(r => r.RiderAnim.Length > 0)} with a rider AnimBP");
        if (o.CheckFlag)
        {
            int differs = 0;
            foreach (var (name, text) in outputs)
            {
                string file = Path.Combine(o.OutDir, name);
                if (!File.Exists(file)) { Console.WriteLine($"{name}: missing in {o.OutDir}"); ++differs; continue; }
                var have = File.ReadAllBytes(file); var fresh = Encoding.UTF8.GetBytes(text);
                if (have.AsSpan().SequenceEqual(fresh)) { Console.WriteLine($"{name}: identical ({have.Length} bytes)"); continue; }
                Console.WriteLine($"{name}: DIFFERS (file {have.Length} bytes, fresh {fresh.Length})");
                ++differs;
            }
            return differs == 0 ? 0 : 1;
        }
        Directory.CreateDirectory(o.OutDir);
        foreach (var (name, text) in outputs)
        {
            string file = Path.Combine(o.OutDir, name);
            File.WriteAllText(file, text);
            Console.WriteLine($"wrote {file}");
        }
        return bad == 0 ? 0 : 1;
    }

    /// <summary>mapc artisan-curves: the profession level-up curves into DIR/artisan_curves.tsv, or with --check a byte comparison with the file that is there.</summary>
    static int ArtisanCurvesCommand(Opt o)
    {
        var game = new Game(o.Paks, o.Usmap);
        var curves = ArtisanCurves.Collect(game, out var packages, out var bad);
        string text = ArtisanCurves.ToTsv(curves);
        Console.WriteLine($"{curves.Count} curves of {packages} packages ({bad} unreadable), {curves.Sum(c => c.Keys.Count)} keys");
        string file = Path.Combine(o.OutDir, ArtisanCurves.FileName);
        if (o.CheckFlag)
        {
            if (!File.Exists(file)) { Console.WriteLine($"{ArtisanCurves.FileName}: missing in {o.OutDir}"); return 1; }
            var have = File.ReadAllBytes(file); var fresh = Encoding.UTF8.GetBytes(text);
            bool same = have.AsSpan().SequenceEqual(fresh);
            Console.WriteLine(same ? $"{ArtisanCurves.FileName}: identical ({have.Length} bytes)" : $"{ArtisanCurves.FileName}: DIFFERS (file {have.Length} bytes, fresh {fresh.Length})");
            return same && bad == 0 ? 0 : 1;
        }
        if (bad != 0) { Console.Error.WriteLine($"mapc: {bad} curve package(s) could not be read: nothing written"); return 1; }
        Directory.CreateDirectory(o.OutDir);
        File.WriteAllText(file, text);
        Console.WriteLine($"wrote {file}");
        return 0;
    }

    /// <summary>What the map read is cut for: the Riverlands' rectangles by default, E1's circle when --center or --radius is given.</summary>
    static Scope MakeScope(Opt o)
    {
        if (o.Circle) return new Scope(o.Cx, o.Cy, o.Radius, o.Margin);
        return new Scope(Region.Load(o.RegionPath ?? Region.DefaultPath()), CenterX, CenterY, RadiusCm, o.Margin);
    }

    /// <summary>Batches of a region read when none is asked for (the cells are cut by package size: see Cells.Split; the first batch's measured peak is in tools/mapc/README.md).</summary>
    public const int DefaultBatches = 12;
    public const double DefaultHeapGb = 8;

    /// <summary>The arguments that make a child read the same scope as this process.</summary>
    static List<string> ScopeArgs(Opt o, Scope scope)
    {
        var a = new List<string> { "--paks", o.Paks, "--usmap", o.Usmap, "--margin", scope.Margin.ToString(CultureInfo.InvariantCulture) };
        if (scope.IsRegion) { a.Add("--region"); a.Add(Path.GetFullPath(scope.Region!.Source)); }
        else { a.Add("--center"); a.Add(FormattableString.Invariant($"{scope.Cx},{scope.Cy}")); a.Add("--radius"); a.Add(scope.Radius.ToString(CultureInfo.InvariantCulture)); }
        return a;
    }

    static string ScopeText(Scope scope) => scope.IsRegion
        ? FormattableString.Invariant($"region {scope.Region!.Hash:X16} margin {scope.Margin:F0}")
        : FormattableString.Invariant($"circle {scope.Cx:F1},{scope.Cy:F1} r {scope.Radius:F0} margin {scope.Margin:F0}");

    /// <summary>mapc extract --cells N/M --part FILE: one batch of the read, in this process; writes the part and its foliage rows (a child of --batches).</summary>
    static int ExtractChild(Opt o)
    {
        var sw = Stopwatch.StartNew();
        var scope = MakeScope(o);
        var game = new Game(o.Paks, o.Usmap);
        var g = Collector.Collect(game, scope, (o.BatchIndex, o.BatchCount), m => Console.WriteLine(m));
        long placedTris = g.Placements.Sum(p => (long)g.Meshes[p.Mesh].Tris);
        string part = o.Part ?? throw new ArgumentException("--cells N/M needs --part FILE");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(part))!);
        File.WriteAllBytes(part, GeomFile.Write(g));
        GeomFile.WriteSrc(part + ".src", g);
        File.WriteAllText(part + ".foliage.tsv", BatchRunner.FoliageTsv(g.Foliage, ScopeText(scope)));
        var proc = Process.GetCurrentProcess();
        Console.WriteLine($"batch {o.BatchIndex}/{o.BatchCount} read in {sw.Elapsed.TotalSeconds:F0} s: {g.Tiles.Count} terrain tiles, {g.Placements.Count} placed meshes of {g.Placements.Select(p => p.Mesh).Distinct().Count()} ({placedTris} triangles), {g.Foliage.Count} gatherable foliage instances; peak working set {proc.PeakWorkingSet64 / 1073741824.0:F2} GB, GC heap {GC.GetGCMemoryInfo().HeapSizeBytes / 1073741824.0:F2} GB");
        foreach (var kv in g.Stats) Console.WriteLine($"  {kv.Key} = {kv.Value}");
        foreach (var w in g.Warnings.Take(20)) Console.WriteLine("  warning: " + w);
        return 0;
    }
// (Cyb | C O A)

    /// <summary>The area's geometry: in this process (E1's circle, or --batches 1), or as child batches whose parts are merged (the region's default).</summary>
    static AreaGeometry Extract(Opt o, string workBase)
    {
        var sw = Stopwatch.StartNew();
        var scope = MakeScope(o);
        if (o.BatchIndex >= 0) throw new ArgumentException("--cells N/M is a batch: use `mapc extract --cells N/M --part FILE`");
        int batches = o.Batches > 0 ? o.Batches : (scope.IsRegion ? DefaultBatches : 0);
        AreaGeometry g;
        if (batches > 1 || (batches == 1 && scope.IsRegion))
        {
            string dll = typeof(Cli).Assembly.Location;
            if (o.StopAfter >= 0) o.KeepParts = true;
            string work = o.WorkDir ?? Path.Combine(workBase, "mapc_batches");
            var stats = BatchRunner.Run(dll, ScopeArgs(o, scope), ScopeText(scope), batches, work, o.HeapGb > 0 ? o.HeapGb : DefaultHeapGb, o.MinFreeGb, o.WaitMinutes, o.StopAfter, m => Console.WriteLine(m), out var parts);
            Console.WriteLine($"merging {parts.Count} parts ...");
            var read = new List<AreaGeometry>();
            foreach (var part in parts)
            {
                var pg = GeomFile.Read(File.ReadAllBytes(part));
                GeomFile.ReadSrc(part + ".src", pg);
                pg.Foliage = BatchRunner.ReadFoliageTsv(part + ".foliage.tsv");
                read.Add(pg);
            }
            g = GeomFile.Merge(read);
            read.Clear();
            Console.WriteLine("batches: " + string.Join("; ", stats.Select(s => s.ToString())));
            var fresh = stats.Where(s => !s.Reused).ToList();
            if (fresh.Count > 0) Console.WriteLine($"batches: slowest {fresh.Max(s => s.Seconds):F0} s, largest peak working set {fresh.Max(s => s.PeakWorkingSetGb):F2} GB, total {fresh.Sum(s => s.Seconds):F0} s");
            if (!o.KeepParts)
            {
                foreach (var part in parts)
                    foreach (var f in new[] { part, part + ".done", part + ".src", part + ".foliage.tsv" })
                        try { File.Delete(f); } catch { }
                try { Directory.Delete(work); } catch { }
            }
        }
        else g = Collector.Collect(new Game(o.Paks, o.Usmap), scope, null, m => Console.WriteLine(m));
        long placedTris = g.Placements.Sum(p => (long)g.Meshes[p.Mesh].Tris);
        Console.WriteLine($"map read in {sw.Elapsed.TotalSeconds:F0} s: {g.Tiles.Count} terrain tiles, {g.Placements.Count} placed meshes of {g.Placements.Select(p => p.Mesh).Distinct().Count()} ({placedTris} triangles), {g.Foliage.Count} gatherable foliage instances");
        foreach (var kv in g.Stats) Console.WriteLine($"  {kv.Key} = {kv.Value}");
        foreach (var w in g.Warnings) Console.WriteLine("  warning: " + w);
        return g;
    }

    /// <summary>area_geom.bin and, for a region, region_foliage.tsv into <paramref name="dir"/>.</summary>
    static void WriteGeometry(Opt o, AreaGeometry g, string dir)
    {
        string geom = Path.Combine(dir, "area_geom.bin");
        File.WriteAllBytes(geom, GeomFile.Write(g));
        Console.WriteLine($"wrote {geom} ({new FileInfo(geom).Length} bytes)");
        if (g.Region != null)
        {
            string fol = Path.Combine(dir, BatchRunner.FoliageFileName);
            File.WriteAllText(fol, BatchRunner.FoliageTsv(g.Foliage, "the region " + g.Region.Hash.ToString("X16", CultureInfo.InvariantCulture)));
            Console.WriteLine($"wrote {fol} ({new FileInfo(fol).Length} bytes, {g.Foliage.Count} instances)");
        }
    }

    static int RunNavbuild(Opt o, string geom, string nav)
    {
        var psi = new ProcessStartInfo(o.Navbuild) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        psi.ArgumentList.Add("--geometry"); psi.ArgumentList.Add(geom);
        psi.ArgumentList.Add("--out"); psi.ArgumentList.Add(nav);
        // the towns' floors: the platforms (mapc town-floors) and the node records they stand at (content.db) are read from the output folder when both are there
        string floors = Path.Combine(o.OutDir, TownFloors.FileName), content = Path.Combine(o.OutDir, "content.db");
        if (File.Exists(floors) && File.Exists(content))
        {
            psi.ArgumentList.Add("--town-floors"); psi.ArgumentList.Add(floors);
            psi.ArgumentList.Add("--content"); psi.ArgumentList.Add(content);
        }
        // the navmesh build runs on the machine's cores; the output does not depend on how many.  Half of them by default (the PC hard-resets under heavy load; a run takes 2 to 3 minutes).
        int threads = o.Threads > 0 ? o.Threads : Math.Max(2, Environment.ProcessorCount / 2);
        psi.ArgumentList.Add("--threads"); psi.ArgumentList.Add(threads.ToString());
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + o.Navbuild);
        var err = p.StandardError.ReadToEndAsync();
        Console.Write(p.StandardOutput.ReadToEnd());
        p.WaitForExit();
        var e = err.Result;
        if (e.Length > 0) Console.Error.Write(e);
        return p.ExitCode;
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static void WriteBlueprints(Opt o, string dir)
    {
        var game = new Game(o.Paks, o.Usmap);
        var rows = NpcBlueprints.Collect(game, NpcBlueprints.AllPaths(game), out var packages, out var bad);
        string motion = Path.Combine(dir, "npc_motion.tsv");
        File.WriteAllText(motion, NpcBlueprints.ToTsv(rows));
        Console.WriteLine($"wrote {motion}: {rows.Count} pawn Blueprints of {packages} packages ({bad} unreadable)");
    }

    /// <summary>Reads the map and writes its geometry files; a method of its own, so that the geometry's memory is free again when it returns.</summary>
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static void ExtractAndWrite(Opt o, string dir)
    {
        var g = Extract(o, dir);
        g = AddTownLayouts(o, g);
        WriteGeometry(o, g, dir);
    }

    /// <summary>The towns' layers (mapc town-layouts: the buildings, walls, terraces and towers of the five bowls) into the region's geometry, when DIR holds the file (like the towns' floors, it is
    /// read from the output folder, not from the scratch folder of --check).  A layer is not read for E1's circle.</summary>
    static AreaGeometry AddTownLayouts(Opt o, AreaGeometry g)
    {
        string file = Path.Combine(o.OutDir, TownLayouts.FileName);
        if (g.Region == null || !File.Exists(file)) return g;
        var sw = Stopwatch.StartNew();
        var layer = TownLayouts.ReadLayer(file);
        int meshes = layer.Meshes.Count, placements = layer.Placements.Count;
        var merged = GeomFile.Merge(new[] { g, layer });
        Console.WriteLine($"town layouts: {meshes} meshes and {placements} placements of {file} merged in {sw.Elapsed.TotalSeconds:F1} s: {merged.Meshes.Count} meshes and {merged.Placements.Count} placements in all");
        return merged;
    }

    static int Build(Opt o, string dir)
    {
        Directory.CreateDirectory(dir);
        // the Blueprints first, in a method of their own: the packages' memory is given back before the map's 45 M triangles are read (the two together
        // peaked at 14 GB and more)
        WriteBlueprints(o, dir);
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        ExtractAndWrite(o, dir);
        GC.Collect();
        string geom = Path.Combine(dir, "area_geom.bin"), nav = Path.Combine(dir, "area_nav.bin");
        int rc = RunNavbuild(o, geom, nav);
        if (rc != 0) return rc;
        Console.WriteLine($"wrote {nav} ({new FileInfo(nav).Length} bytes)");
        return 0;
    }
// built with C O A and C | Y | B

    /// <summary>area_nav.bin of a region is chunked (version 2): two files are the same when every chunk's hash, the terrain and the header agree (aoc_navbuild --compare says which chunk
    /// differs); E1's version 1 is compared byte for byte.</summary>
    static bool SameNav(Opt o, string have, string fresh)
    {
        static bool IsChunked(string path)
        {
            using var f = File.OpenRead(path);
            var b = new byte[8];
            return f.Read(b, 0, 8) == 8 && Encoding.ASCII.GetString(b) == "AOCNAV02";
        }
        if (IsChunked(have) && IsChunked(fresh))
        {
            var psi = new ProcessStartInfo(o.Navbuild) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
            psi.ArgumentList.Add("--compare"); psi.ArgumentList.Add(have); psi.ArgumentList.Add(fresh);
            using var p = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + o.Navbuild);
            var err = p.StandardError.ReadToEndAsync();
            Console.Write(p.StandardOutput.ReadToEnd());
            p.WaitForExit();
            if (err.Result.Length > 0) Console.Error.Write(err.Result);
            return p.ExitCode == 0;
        }
        var a = new FileInfo(have); var b = new FileInfo(fresh);
        if (a.Length != b.Length) return false;
        using var fa = File.OpenRead(have); using var fb = File.OpenRead(fresh);
        var ba = new byte[1 << 20]; var bb = new byte[1 << 20];
        while (true)
        {
            int na = fa.Read(ba, 0, ba.Length), nb = fb.Read(bb, 0, bb.Length);
            if (na != nb) return false;
            if (na == 0) return true;
            if (!ba.AsSpan(0, na).SequenceEqual(bb.AsSpan(0, nb))) return false;
        }
    }

    static bool SameBytes(string have, string fresh)
    {
        var a = new FileInfo(have); var b = new FileInfo(fresh);
        if (a.Length != b.Length) return false;
        using var fa = File.OpenRead(have); using var fb = File.OpenRead(fresh);
        var ba = new byte[1 << 20]; var bb = new byte[1 << 20];
        while (true)
        {
            int na = fa.Read(ba, 0, ba.Length), nb = fb.Read(bb, 0, bb.Length);
            if (na != nb) return false;
            if (na == 0) return true;
            if (!ba.AsSpan(0, na).SequenceEqual(bb.AsSpan(0, nb))) return false;
        }
    }

    static int Check(Opt o)
    {
        string tmp = Path.Combine(Path.GetTempPath(), "mapc_check_" + Environment.ProcessId);
        try
        {
            int rc = Build(o, tmp);
            if (rc != 0) return rc;
            int bad = 0;
            var names = new List<string> { "area_geom.bin", "area_nav.bin", "npc_motion.tsv" };
            if (!o.Circle) names.Add(BatchRunner.FoliageFileName);
            foreach (var name in names)
            {
                string have = Path.Combine(o.OutDir, name), fresh = Path.Combine(tmp, name);
                if (!File.Exists(have)) { Console.WriteLine($"{name}: missing in {o.OutDir}"); bad++; continue; }
                bool same = name == "area_nav.bin" ? SameNav(o, have, fresh) : SameBytes(have, fresh);
                long la = new FileInfo(have).Length, lb = new FileInfo(fresh).Length;
                if (same) Console.WriteLine($"{name}: identical ({la} bytes{(name == "area_nav.bin" ? ", chunk hashes" : "")})");
                else { Console.WriteLine($"{name}: DIFFERS (file {la} bytes, rebuilt {lb})"); bad++; }
            }
            return bad == 0 ? 0 : 1;
        }
        finally { try { Directory.Delete(tmp, true); } catch { } }
    }
}
