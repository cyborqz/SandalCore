// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text.RegularExpressions;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>One streaming cell of the World Partition level the area touches.</summary>
public sealed record CellInfo(string Id, string Grid, int Level, int X, int Y, long Size, double CenterX = 0, double CenterY = 0)
{
    /// <summary>The grids whose cells hold what the ground is made of (the HLOD grids Main0.., Foliage0.. are visual proxies of the real ones).</summary>
    public bool IsReadGrid => Grid is "MainGrid" or "Foliage" or "Landscape" or "Water";
}

public static class Cells
{
    static readonly Regex Name = new(@"_L(\d+)_X(-?\d+)_Y(-?\d+)$", RegexOptions.Compiled);

    /// <summary>The ids of the cells the scope touches, sorted by name, and each one's grid (E1's circle: the old call).</summary>
    public static IEnumerable<string> Hit(Game g, double cx, double cy, double radius, double margin, out Dictionary<string, string> grids)
        => Hit(g, new Scope(cx, cy, radius, margin), out grids);

    public static IEnumerable<string> Hit(Game g, Scope scope, out Dictionary<string, string> grids)
    {
        var main = g.Load(Game.MasterPath + ".umap")!;
        var res = new List<string>();
        grids = new();
        foreach (var e in main.GetExports().Where(x => x.ExportType == "WorldPartitionRuntimeCellDataHashSet"))
        {
            var cb = e.GetOrDefault<FBox>("ContentBounds");
            if (scope.CellHit(cb.Min.X, cb.Min.Y, cb.Max.X, cb.Max.Y))
            {
                var id = e.Outer!.Name;
                res.Add(id);
                grids[id] = e.GetOrDefault<FName>("GridName").Text;
            }
        }
        res.Sort(StringComparer.Ordinal);
        return res;
    }
// [C.O.A x Cyb]

    /// <summary>Every cell the scope touches, with its grid, its level and position (from the name), its content bounds' centre and the size of its package in the containers (0: the map names a cell the containers lack).</summary>
    public static List<CellInfo> Describe(Game g, Scope scope)
    {
        var main = g.Load(Game.MasterPath + ".umap")!;
        var list = new List<CellInfo>();
        foreach (var e in main.GetExports().Where(x => x.ExportType == "WorldPartitionRuntimeCellDataHashSet"))
        {
            var cb = e.GetOrDefault<FBox>("ContentBounds");
            if (!scope.CellHit(cb.Min.X, cb.Min.Y, cb.Max.X, cb.Max.Y)) continue;
            var id = e.Outer!.Name;
            var m = Name.Match(id);
            int lvl = 0, x = 0, y = 0;
            if (m.Success) { lvl = int.Parse(m.Groups[1].Value); x = int.Parse(m.Groups[2].Value); y = int.Parse(m.Groups[3].Value); }
            long size = 0;
            if (g.Provider.TryGetGameFile(PathOf(id), out var f)) size = f.Size;
            list.Add(new CellInfo(id, e.GetOrDefault<FName>("GridName").Text, lvl, x, y, size, (cb.Min.X + cb.Max.X) * 0.5, (cb.Min.Y + cb.Max.Y) * 0.5));
        }
        list.Sort((a, b) => string.CompareOrdinal(a.Id, b.Id));
        return list;
    }

    /// <summary>
    /// The batches: the cells that are read (not the HLOD proxies), in a spatial order (bands of 256 m from the south, then west to east: every batch is a strip of the map with all four
    /// grids in it, a typical load), cut into <paramref name="count"/> contiguous runs of about equal package size (a batch is one child process under a heap limit; neighbours share
    /// meshes, so a run reads fewer of them again).  The cut is a function of the cell list alone: every child computes the same one.
    /// </summary>
    public static List<CellInfo>[] Split(IEnumerable<CellInfo> read, int count)
    {
        var order = read.Where(c => c.IsReadGrid).OrderBy(c => Math.Floor(c.CenterY / 25600.0)).ThenBy(c => c.CenterX).ThenBy(c => GridRank(c.Grid)).ThenBy(c => c.Id, StringComparer.Ordinal).ToList();
        var parts = new List<CellInfo>[count];
        for (int i = 0; i < count; i++) parts[i] = new();
        // a cell weighs its package plus a flat 64 KB, so that a run of empty cells is not free
        double total = order.Sum(c => c.Size + 65536.0), run = 0;
        foreach (var c in order)
        {
            double w = c.Size + 65536.0;
            int b = (int)Math.Min(count - 1, Math.Floor((run + w * 0.5) * count / total));
            parts[b].Add(c);
            run += w;
        }
        return parts;
    }

    static int GridRank(string grid) => grid switch { "Landscape" => 0, "Water" => 1, "MainGrid" => 2, "Foliage" => 3, _ => 4 };

    public static string PathOf(string id) => $"{Game.MasterPath}/_Generated_/{id}.umap";
}
