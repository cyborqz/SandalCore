// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Diagnostics;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using Newtonsoft.Json;

namespace Mapc;

/// <summary>Cheap diagnostics: they read the master level alone, or one cell.</summary>
public static class Diag
{
    /// <summary>
    /// mapc probe-cells: the master umap alone (a few seconds): the cells the scope reads, per grid, with their package sizes, the largest ones, and how a --batches M run would cut them.
    /// </summary>
    public static int ProbeCells(Game g, Scope scope, int batches, Action<string> log)
    {
        var sw = Stopwatch.StartNew();
        log("scope: " + scope.Describe());
        var all = Cells.Describe(g, scope);
        log($"master umap read in {sw.Elapsed.TotalSeconds:F1} s: {all.Count} cells touch the scope");
        var read = all.Where(c => c.IsReadGrid).ToList();
        foreach (var grp in all.GroupBy(c => c.Grid).OrderBy(x => x.Key, StringComparer.Ordinal))
        {
            bool isRead = grp.First().IsReadGrid;
            long bytes = grp.Sum(c => c.Size);
            int missing = grp.Count(c => c.Size == 0);
            log($"  {grp.Key,-12} {grp.Count(),5} cells{(isRead ? "" : " (HLOD proxy, not read)"),-26} {bytes / 1048576.0,9:F1} MB of packages, {missing} not in the containers, levels {string.Join("/", grp.GroupBy(c => c.Level).OrderBy(x => x.Key).Select(x => $"L{x.Key}:{x.Count()}"))}");
        }
        log($"read set: {read.Count} cells (MainGrid {read.Count(c => c.Grid == "MainGrid")}, Foliage {read.Count(c => c.Grid == "Foliage")}, Landscape {read.Count(c => c.Grid == "Landscape")}, Water {read.Count(c => c.Grid == "Water")}), {read.Sum(c => c.Size) / 1048576.0:F0} MB of packages");
        foreach (var c in read.OrderByDescending(c => c.Size).Take(8)) log($"  largest: {c.Id} {c.Size / 1048576.0:F1} MB");
        if (batches > 0)
        {
            var parts = Cells.Split(read, batches);
            for (int i = 0; i < parts.Length; i++)
                log($"  batch {i}/{batches}: {parts[i].Count} cells ({parts[i].Count(c => c.Grid == "MainGrid")} MainGrid, {parts[i].Count(c => c.Grid == "Foliage")} Foliage, {parts[i].Count(c => c.Grid == "Landscape")} Landscape, {parts[i].Count(c => c.Grid == "Water")} Water), {parts[i].Sum(c => c.Size) / 1048576.0:F0} MB");
        }
        return 0;
    }

    /// <summary>mapc foliage-probe: the Foliage cells nearest the login spot, the types of their exports and what a gatherable foliage component holds (diagnostic).</summary>
    public static int FoliageProbe(Game g, Scope scope, int cellsToShow)
    {
        var all = Cells.Describe(g, scope).Where(c => c.Grid == "Foliage" && c.Size > 0).OrderBy(c => Math.Sqrt(Math.Pow(c.CenterX - Cli.CenterX, 2) + Math.Pow(c.CenterY - Cli.CenterY, 2))).Take(cellsToShow).ToList();
        foreach (var c in all)
        {
            Console.WriteLine($"==== {c.Id} {c.Size / 1024} KB centre ({c.CenterX:F0}, {c.CenterY:F0})");
            var pkg = g.Load(Cells.PathOf(c.Id));
            if (pkg == null) continue;
            var types = new SortedDictionary<string, int>(StringComparer.Ordinal);
            var shown = 0;
            foreach (var e in pkg.GetExports())
            {
                types[e.ExportType] = types.GetValueOrDefault(e.ExportType) + 1;
                if ((e.ExportType.Contains("Gatherable", StringComparison.OrdinalIgnoreCase) || e.Name.Contains("Gatherable", StringComparison.OrdinalIgnoreCase)) && shown++ < 2)
                {
                    Console.WriteLine($"-- {e.ExportType} {e.Name} (outer {e.Outer?.Name} {e.Outer?.ExportType}) class {e.Class?.Name}");
                    Console.WriteLine("   props: " + string.Join(",", e.Properties.Select(p => p.Name.Text)));
                    try
                    {
                        var js = JsonConvert.SerializeObject(e.Properties.ToDictionary(p => p.Name.Text, p => p.Tag?.GenericValue), Formatting.None,
                            new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s2, a) => a.ErrorContext.Handled = true });
                        Console.WriteLine(js.Length > 3000 ? js[..3000] + "..." : js);
                    }
                    catch (Exception ex) { Console.WriteLine("  (props: " + ex.Message + ")"); }
                }
            }
            Console.WriteLine("   types: " + string.Join(", ", types.OrderByDescending(k => k.Value).Take(14).Select(k => k.Key + " x" + k.Value)));
            g.Forget(Cells.PathOf(c.Id));
        }
        return 0;
    }
// -- CoA . C_Y_B --

    /// <summary>mapc foliage-detail CELLID: every IntrepidGatherableFISMC of a cell: its C# type, DDERecordName/Id, tag map, mesh and instances (diagnostic).</summary>
    public static int FoliageDetail(Game g, string cellId, int maxComponents)
    {
        var pkg = g.Load(Cells.PathOf(cellId));
        if (pkg == null) { Console.WriteLine("not loadable"); return 1; }
        int n = 0;
        foreach (var e in pkg.GetExports())
        {
            if (!e.ExportType.StartsWith("IntrepidGatherableFISMC", StringComparison.Ordinal)) continue;
            if (n++ >= maxComponents) break;
            Console.WriteLine($"-- {e.Name}: C# {e.GetType().Name}, outer {e.Outer?.Name}");
            foreach (var p in e.Properties)
            {
                if (p.Name.Text is "SortedInstances" or "InstanceReorderTable" or "CachedBounds" or "BodyInstance") { Console.WriteLine($"   {p.Name.Text}: {p.Tag?.GetType().Name}"); continue; }
                string v;
                try { v = JsonConvert.SerializeObject(p.Tag?.GenericValue, Formatting.None, new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s2, a) => a.ErrorContext.Handled = true }); }
                catch (Exception ex) { v = "(" + ex.Message + ")"; }
                Console.WriteLine($"   {p.Name.Text} [{p.Tag?.GetType().Name}] = {(v.Length > 300 ? v[..300] + "..." : v)}");
            }
            if (e is CUE4Parse.UE4.Assets.Exports.Component.StaticMesh.UInstancedStaticMeshComponent ism)
            {
                var inst = ism.GetInstances();
                Console.WriteLine($"   instances: {inst.Length}; mesh {Lv.PathOf(ism.GetStaticMesh())}");
                foreach (var i1 in inst.Take(3)) Console.WriteLine($"     t=({i1.TransformData.Translation.X:F1},{i1.TransformData.Translation.Y:F1},{i1.TransformData.Translation.Z:F1}) s=({i1.TransformData.Scale3D.X:F2},{i1.TransformData.Scale3D.Y:F2},{i1.TransformData.Scale3D.Z:F2})");
                var w = Lv.World(e);
                Console.WriteLine($"   component world M03/M13/M23 = {w.M03:F1} {w.M13:F1} {w.M23:F1}, scale {w.ScaleX:F2}");
            }
        }
        return 0;
    }

    /// <summary>mapc cell-exports CELLID [TYPE-OR-NAME-SUBSTRING]: the export types of one cell with their counts, and the first few exports whose type or name contains the filter.</summary>
    public static int CellExports(Game g, string cellId, string? filter, int show)
    {
        var pkg = g.Load(Cells.PathOf(cellId));
        if (pkg == null) { Console.WriteLine("cell not loadable"); return 1; }
        var byType = new SortedDictionary<string, int>(StringComparer.Ordinal);
        if (pkg is IoPackage io)
        {
            for (int i = 0; i < io.ExportMap.Length; i++)
            {
                string name = io.CreateFNameFromMappedName(io.ExportMap[i].ObjectName).Text;
                int cut = name.LastIndexOf('_');
                string key = filter != null && name.Contains(filter, StringComparison.OrdinalIgnoreCase) ? name : "(all)";
                if (key != "(all)") byType[key] = byType.GetValueOrDefault(key) + 1;
            }
        }
        foreach (var kv in byType.Take(show)) Console.WriteLine($"  export name {kv.Key} x{kv.Value}");
        int shown = 0;
        foreach (var e in pkg.GetExports())
        {
            byType[e.ExportType] = byType.GetValueOrDefault(e.ExportType) + 1;
        }
        foreach (var kv in byType.Where(k => !k.Key.Contains('_') || true).OrderByDescending(k => k.Value).Take(40)) Console.WriteLine($"  type {kv.Key}: {kv.Value}");
        if (filter != null)
        {
            foreach (var e in pkg.GetExports())
            {
                if (!e.ExportType.Contains(filter, StringComparison.OrdinalIgnoreCase) && !e.Name.Contains(filter, StringComparison.OrdinalIgnoreCase)) continue;
                if (shown++ >= show) break;
                Console.WriteLine($"-- {e.ExportType} {e.Name} (outer {e.Outer?.Name} {e.Outer?.ExportType})");
                try
                {
                    var js = JsonConvert.SerializeObject(e.Properties.ToDictionary(p => p.Name.Text, p => p.Tag?.GenericValue), Formatting.None,
                        new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s, a) => a.ErrorContext.Handled = true });
                    Console.WriteLine(js.Length > 4000 ? js[..4000] + "..." : js);
                }
                catch (Exception ex) { Console.WriteLine("  (props: " + ex.Message + ")"); }
            }
        }
        return 0;
    }
}
